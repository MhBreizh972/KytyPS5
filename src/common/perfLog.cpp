#include "common/perfLog.h"

#include "common/debugCounters.h"
#include "common/hostStats.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>

namespace Common::DebugCounters {

const char* Name(Counter counter) noexcept {
	static constexpr const char* NAMES[] = {
	    "draws",
	    "draw_instances",
	    "indirect_draws",
	    "dispatches",
	    "queue_submits",
	    "gpu_waits",
	    "gpu_wait_ns",
	    "gpu_faults",
	    "shaders_compiled",
	    "shader_compile_ns",
	    "pipelines_created",
	    "pipeline_create_ns",
	    "texture_upload_bytes",
	    "texture_download_bytes",
	    "texture_evictions",
	    "color_metadata_readbacks",
	    "color_metadata_readback_ns",
	    "color_metadata_checks",
	    "buffer_upload_bytes",
	    "buffer_download_bytes",
	    "buffer_evictions",
	    "flips_requested",
	    "flips_rejected",
	    "late_vblanks",
	    "swapchain_recreations",
	    "dropped_presents",
	    "emulated_instructions",
	    "unresolved_import_calls",
	    "guest_file_read_bytes",
	    "guest_file_write_bytes",
	    "audio_underruns",
	    "gpu_thread_busy_ns",
	    "present_ns",
	    "present_acquire_ns",
	    "overlay_ns",
	    "gpu_exec_ns",
	    "gpu_timed_submits",
	    "pipeline_barriers",
	    "image_copies",
	    "buffer_copies",
	    "texture_creations",
	    "texture_invalidations",
	    "occlusion_dumps",
	    "occlusion_synthetic_dumps",
	    "occlusion_publishes",
	    "wait_finish_ns",
	    "wait_occlusion_ns",
	    "wait_stream_ns",
	    "wait_readback_ns",
	    "wait_fault_buffer_ns",
	    "wait_write_back_ns",
	    "wait_present_ns",
	    "wait_other_ns",
	    "cp_blocked_sleeps",
	    "cp_blocked_sleep_ns",
	};
	static_assert(std::size(NAMES) == static_cast<size_t>(Counter::Count),
	              "every debug counter needs a CSV column name");
	const auto index = static_cast<size_t>(counter);
	return index < std::size(NAMES) ? NAMES[index] : "unknown";
}

} // namespace Common::DebugCounters

namespace Common::PerfLog {

namespace {

using Common::DebugCounters::Counter;
constexpr size_t COUNTERS        = static_cast<size_t>(Counter::Count);
constexpr size_t LOGGED_THREADS  = 4;
std::jthread     g_thread;
std::mutex       g_mutex;

std::string CsvField(std::string text) {
	for (auto& c: text) {
		if (c == ',' || c == '"' || c == '\n' || c == '\r') {
			c = ' ';
		}
	}
	return text;
}

void Run(const std::stop_token& stop, FILE* file, std::chrono::milliseconds interval) {
	std::fprintf(file, "time_s,interval_s");
	for (size_t i = 0; i < COUNTERS; i++) {
		std::fprintf(file, ",%s_per_s", Common::DebugCounters::Name(static_cast<Counter>(i)));
	}
	std::fprintf(file, ",process_cpu_percent,logical_cores");
	for (size_t i = 0; i < LOGGED_THREADS; i++) {
		std::fprintf(file, ",thread%zu_name,thread%zu_core_percent", i, i);
	}
	std::fprintf(file, "\n");
	std::fflush(file);

	Common::HostStatsSampler sampler;
	(void)sampler.Sample(); // The first sample has no rates.

	using Clock      = std::chrono::steady_clock;
	const auto start = Clock::now();
	auto       last  = start;
	std::array<uint64_t, COUNTERS> previous {};
	for (size_t i = 0; i < COUNTERS; i++) {
		previous[i] = Common::DebugCounters::Get(static_cast<Counter>(i));
	}

	auto next = start + interval;
	while (!stop.stop_requested()) {
		std::this_thread::sleep_until(next);
		next += interval;
		const auto now     = Clock::now();
		const auto elapsed = std::chrono::duration<double>(now - last).count();
		last               = now;
		if (elapsed <= 0.0) {
			continue;
		}
		std::fprintf(file, "%.3f,%.3f", std::chrono::duration<double>(now - start).count(),
		             elapsed);
		for (size_t i = 0; i < COUNTERS; i++) {
			const auto value = Common::DebugCounters::Get(static_cast<Counter>(i));
			std::fprintf(file, ",%.1f", static_cast<double>(value - previous[i]) / elapsed);
			previous[i] = value;
		}
		const auto host = sampler.Sample();
		std::fprintf(file, ",%.1f,%u", host.process_cpu_percent.value_or(-1.0),
		             host.logical_cores);
		for (size_t i = 0; i < LOGGED_THREADS; i++) {
			if (i < host.busiest_threads.size()) {
				std::fprintf(file, ",%s,%.1f", CsvField(host.busiest_threads[i].name).c_str(),
				             host.busiest_threads[i].cpu_percent);
			} else {
				std::fprintf(file, ",,");
			}
		}
		std::fprintf(file, "\n");
		std::fflush(file);
	}
	std::fclose(file);
}

} // namespace

void Start() {
	std::lock_guard lock(g_mutex);
	if (g_thread.joinable()) {
		return;
	}
	const char* path = std::getenv("KYTY_PERF_CSV");
	if (path == nullptr || *path == '\0') {
		return;
	}
	long interval_ms = 1000;
	if (const char* text = std::getenv("KYTY_PERF_CSV_INTERVAL_MS"); text != nullptr) {
		interval_ms = std::strtol(text, nullptr, 10);
		if (interval_ms < 100) {
			interval_ms = 100;
		}
	}
	FILE* file = std::fopen(path, "w");
	if (file == nullptr) {
		std::printf("KYTY_PERF_CSV: cannot open %s\n", path);
		return;
	}
	std::printf("KYTY_PERF_CSV: logging performance counters to %s every %ld ms\n", path,
	            interval_ms);
	Common::DebugCounters::SetLogging(true);
	g_thread = std::jthread([file, interval_ms](const std::stop_token& stop) {
		Run(stop, file, std::chrono::milliseconds(interval_ms));
	});
}

void Stop() {
	std::lock_guard lock(g_mutex);
	if (g_thread.joinable()) {
		g_thread.request_stop();
		g_thread.join();
	}
	Common::DebugCounters::SetLogging(false);
}

} // namespace Common::PerfLog
