#!/usr/bin/env python3
"""Summarize KYTY_PERF_CSV logs and PresentMon captures for the performance lab.

Usage:
  analyze_perf_csv.py run.csv --last 60      (the 60 s before the last 3 s of the log)
  analyze_perf_csv.py run.csv [--from 30 --to 90]
  analyze_perf_csv.py baseline.csv candidate.csv [--from 30 --to 90]
  analyze_perf_csv.py run.csv --from 0 --to 120 --trend   (what grows while FPS falls)
  analyze_perf_csv.py --presentmon capture.csv [other.csv ...]

Rates in the CSV are per second; this script reports them per flip, which is per guest frame.
Only the time window [--from, --to] (seconds since the log started) is summarized, so loading
and menus can be left out. Use the same scene, position and window for every compared run.
"""

import argparse
import csv
import math
import statistics
import sys

MS = 1e-6  # ns -> ms

WAIT_COLUMNS = [
    ("full drains (Finish)", "wait_finish_ns_per_s"),
    ("occlusion publication", "wait_occlusion_ns_per_s"),
    ("stream ring wrap", "wait_stream_ns_per_s"),
    ("buffer readback/evict", "wait_readback_ns_per_s"),
    ("fault buffer", "wait_fault_buffer_ns_per_s"),
    ("image write-backs", "wait_write_back_ns_per_s"),
    ("present", "wait_present_ns_per_s"),
    ("other", "wait_other_ns_per_s"),
]


def load(path, start, end, last=None):
    with open(path, newline="") as f:
        rows = [r for r in csv.DictReader(f)]
    if last is not None and rows:
        # The final seconds include shutdown; leave them out.
        stop = float(rows[-1]["time_s"]) - 3.0
        start, end = stop - last, stop
    picked = [r for r in rows if start <= float(r["time_s"]) <= end]
    if not picked:
        sys.exit(f"{path}: no rows between {start} and {end} s ({len(rows)} rows in total)")
    return picked


def mean(rows, column):
    values = [float(r[column]) for r in rows if r.get(column, "") not in ("", None)]
    return statistics.fmean(values) if values else 0.0


def summarize(path, rows):
    s = {"path": path, "seconds": len(rows)}
    fps = mean(rows, "flips_requested_per_s")
    s["fps"] = fps
    per_frame = (lambda c: mean(rows, c) / fps) if fps > 0 else (lambda c: 0.0)
    s["frame_ms"] = 1000.0 / fps if fps > 0 else float("nan")
    s["gpu_exec_ms"] = per_frame("gpu_exec_ns_per_s") * MS
    s["gpu_thread_busy_ms"] = per_frame("gpu_thread_busy_ns_per_s") * MS
    s["gpu_wait_ms"] = per_frame("gpu_wait_ns_per_s") * MS
    s["waits"] = [(label, per_frame(c) * MS) for label, c in WAIT_COLUMNS]
    s["cp_blocked_ms"] = per_frame("cp_blocked_sleep_ns_per_s") * MS
    s["cp_blocked_sleeps"] = per_frame("cp_blocked_sleeps_per_s")
    s["compile_ms"] = (per_frame("shader_compile_ns_per_s") + per_frame("pipeline_create_ns_per_s")) * MS
    s["present_ms"] = per_frame("present_ns_per_s") * MS
    for key in ["draws", "dispatches", "queue_submits", "pipeline_barriers", "image_copies",
                "buffer_copies", "texture_creations", "texture_invalidations", "gpu_faults",
                "occlusion_dumps", "occlusion_synthetic_dumps", "occlusion_publishes",
                "idle_flushes", "gpu_waits", "texture_upload_bytes", "buffer_upload_bytes",
                "texture_download_bytes", "buffer_download_bytes"]:
        s[key] = per_frame(f"{key}_per_s")
    s["process_cpu"] = mean(rows, "process_cpu_percent")
    threads = {}
    for r in rows:
        for i in range(4):
            name = r.get(f"thread{i}_name", "")
            if name:
                threads.setdefault(name, []).append(float(r[f"thread{i}_core_percent"]))
    # A thread absent from a row's top four counted as idle there.
    s["threads"] = sorted(((n, sum(v) / len(rows)) for n, v in threads.items()),
                          key=lambda t: -t[1])[:6]
    return s


def verdict(s):
    lines = []
    frame = s["frame_ms"]
    if not math.isfinite(frame):
        return ["no flips in the window"]
    gpu_share = s["gpu_exec_ms"] / frame
    top = s["threads"][0] if s["threads"] else ("?", 0.0)
    lines.append(f"host GPU busy {gpu_share:.0%} of the frame "
                 f"({s['gpu_exec_ms']:.1f} of {frame:.1f} ms)")
    if gpu_share > 0.85:
        lines.append("=> GPU-bound: look at shader cost, resolution, passes and copies")
    if top[1] > 85:
        lines.append(f"=> one host thread is saturated: '{top[0]}' at {top[1]:.0f}% of a core")
    if s["gpu_wait_ms"] > 0.2 * frame:
        worst = max(s["waits"], key=lambda w: w[1])
        lines.append(f"=> CPU blocked on the GPU {s['gpu_wait_ms']:.1f} ms/frame, mostly "
                     f"'{worst[0]}' ({worst[1]:.1f} ms): CPU and GPU take turns")
    if s["cp_blocked_ms"] > 0.1 * frame:
        lines.append(f"=> guest queues suspended {s['cp_blocked_ms']:.1f} ms/frame "
                     f"({s['cp_blocked_sleeps']:.0f} sleeps/frame)")
    if s["occlusion_synthetic_dumps"] > 0:
        lines.append(f"=> {s['occlusion_synthetic_dumps']:.0f} occlusion results/frame answered "
                     "'always visible' (--occlusion-queries off): the game draws what it would cull")
    if s["compile_ms"] > 0.05 * frame:
        lines.append(f"=> shader/pipeline compilation {s['compile_ms']:.1f} ms/frame in the window")
    if gpu_share < 0.6 and top[1] < 85 and s["gpu_wait_ms"] < 0.2 * frame:
        lines.append("=> neither side saturated nor a dominant wait: check guest-side pacing "
                     "(the game's own CPU threads and its frame limiter)")
    return lines


def print_one(s):
    print(f"== {s['path']}  ({s['seconds']} samples)")
    print(f"  FPS (flips/s)            {s['fps']:8.2f}   frame {s['frame_ms']:.2f} ms")
    print(f"  GPU execution            {s['gpu_exec_ms']:8.2f} ms/frame (Vulkan timestamps)")
    print(f"  Guest GPU thread busy    {s['gpu_thread_busy_ms']:8.2f} ms/frame (includes its waits)")
    print(f"  CPU waits on GPU         {s['gpu_wait_ms']:8.2f} ms/frame, {s['gpu_waits']:.1f} waits")
    for label, ms in s["waits"]:
        if ms >= 0.01:
            print(f"    {label:24s} {ms:8.2f} ms")
    print(f"  Guest queues suspended   {s['cp_blocked_ms']:8.2f} ms/frame, "
          f"{s['cp_blocked_sleeps']:.1f} sleeps")
    print(f"  Shader+pipeline compile  {s['compile_ms']:8.2f} ms/frame")
    print(f"  Present                  {s['present_ms']:8.2f} ms/frame")
    print(f"  Draws {s['draws']:.0f}  dispatches {s['dispatches']:.0f}  submits {s['queue_submits']:.1f}"
          f"  idle flushes {s['idle_flushes']:.1f}  barriers {s['pipeline_barriers']:.0f}")
    print(f"  Copies: image {s['image_copies']:.1f}  buffer {s['buffer_copies']:.1f}   "
          f"textures created {s['texture_creations']:.2f}  invalidated {s['texture_invalidations']:.1f}"
          f"  GPU-page faults {s['gpu_faults']:.1f}")
    print(f"  Uploads: textures {s['texture_upload_bytes'] / 2**20:.2f} MiB  buffers "
          f"{s['buffer_upload_bytes'] / 2**20:.2f} MiB   downloads "
          f"{(s['texture_download_bytes'] + s['buffer_download_bytes']) / 2**20:.2f} MiB")
    print(f"  Occlusion: host dumps {s['occlusion_dumps']:.1f}  synthetic "
          f"{s['occlusion_synthetic_dumps']:.1f}  publications {s['occlusion_publishes']:.1f}")
    print(f"  Process CPU              {s['process_cpu']:8.1f} % of all logical cores")
    for name, pct in s["threads"]:
        print(f"    {name:28s} {pct:5.0f} % of a core")
    for line in verdict(s):
        print("  " + line)


TREND_KEYS = ["fps", "gpu_exec_ms", "gpu_thread_busy_ms", "gpu_wait_ms", "cp_blocked_ms",
              "draws", "queue_submits", "gpu_faults", "texture_invalidations",
              "texture_creations", "image_copies", "compile_ms"]


def trend(path, rows):
    """Splits the window in thirds: a cost that grows while FPS falls is a candidate cause of a
    slowdown over time (for example after standing still for 30 s)."""
    if len(rows) < 9:
        return
    third = len(rows) // 3
    first = summarize(path, rows[:third])
    last = summarize(path, rows[-third:])
    print(f"== trend in {path}: first {third} s vs last {third} s of the window")
    for key in TREND_KEYS:
        x, y = first[key], last[key]
        if x == 0 and y == 0:
            continue
        delta = (y - x) / x * 100 if x else float("inf")
        flag = "  <==" if abs(delta) > 25 else ""
        print(f"  {key:22s} {x:9.2f} -> {y:9.2f}  ({delta:+.0f}%){flag}")


def compare(a, b):
    print("== candidate vs baseline (per frame)")
    rows = [("FPS", "fps"), ("frame ms", "frame_ms"), ("GPU exec ms", "gpu_exec_ms"),
            ("GPU thread busy ms", "gpu_thread_busy_ms"), ("CPU waits ms", "gpu_wait_ms"),
            ("queues suspended ms", "cp_blocked_ms"), ("submits", "queue_submits"),
            ("barriers", "pipeline_barriers"), ("draws", "draws")]
    for label, key in rows:
        x, y = a[key], b[key]
        delta = (y - x) / x * 100 if x else float("nan")
        print(f"  {label:22s} {x:9.2f} -> {y:9.2f}  ({delta:+.1f}%)")
    if abs(a["draws"] - b["draws"]) > 0.05 * max(a["draws"], 1):
        print("  WARNING: draws/frame differ by more than 5%: the runs did not render the same work")


def presentmon(paths):
    for path in paths:
        with open(path, newline="") as f:
            reader = csv.DictReader(f)
            column = next((c for c in ("MsBetweenPresents", "FrameTime", "msBetweenPresents")
                           if c in (reader.fieldnames or [])), None)
            if column is None:
                sys.exit(f"{path}: no MsBetweenPresents/FrameTime column")
            times = [float(r[column]) for r in reader if r[column] not in ("", "NA")]
        times = [t for t in times if t > 0]
        if not times:
            sys.exit(f"{path}: no frames")
        times.sort()
        total = sum(times)
        worst = times[-max(1, len(times) // 100):]
        p = lambda q: times[min(len(times) - 1, int(q * len(times)))]
        print(f"== {path}: {len(times)} frames over {total / 1000:.1f} s")
        print(f"  average FPS   {1000 * len(times) / total:7.2f}")
        print(f"  1% low FPS    {1000 / statistics.fmean(worst):7.2f}  (mean of the slowest 1%)")
        print(f"  min FPS       {1000 / times[-1]:7.2f}  (slowest frame)")
        print(f"  frame time p50 {p(0.5):.2f}  p95 {p(0.95):.2f}  p99 {p(0.99):.2f} ms")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("csv", nargs="+")
    parser.add_argument("--from", dest="start", type=float, default=0.0)
    parser.add_argument("--to", dest="end", type=float, default=1e12)
    parser.add_argument("--last", type=float, default=None,
                        help="summarize the N seconds before the last 3 s of each log")
    parser.add_argument("--trend", action="store_true",
                        help="compare the first and last third of each window")
    parser.add_argument("--presentmon", action="store_true")
    args = parser.parse_args()
    if args.presentmon:
        presentmon(args.csv)
        return
    loaded = [(p, load(p, args.start, args.end, args.last)) for p in args.csv]
    summaries = [summarize(p, rows) for p, rows in loaded]
    for s in summaries:
        print_one(s)
    if args.trend:
        for p, rows in loaded:
            trend(p, rows)
    if len(summaries) == 2:
        compare(*summaries)


if __name__ == "__main__":
    main()
