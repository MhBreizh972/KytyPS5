#ifndef KYTY_COMMON_PERF_LOG_H_
#define KYTY_COMMON_PERF_LOG_H_

namespace Common::PerfLog {

// Performance lab log. With KYTY_PERF_CSV=<path>, debug counters stay enabled and a background
// thread appends one CSV row per interval (KYTY_PERF_CSV_INTERVAL_MS, default 1000): every
// counter as a per-second rate, process CPU and the busiest host threads. Frame pacing is left to
// an external capture (PresentMon), which measures presentation without emulator involvement.
void Start();
void Stop();

} // namespace Common::PerfLog

#endif /* KYTY_COMMON_PERF_LOG_H_ */
