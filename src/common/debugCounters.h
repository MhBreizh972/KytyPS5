#ifndef KYTY_COMMON_DEBUG_COUNTERS_H_
#define KYTY_COMMON_DEBUG_COUNTERS_H_

#include <atomic>
#include <chrono>
#include <cstdint>

namespace Common::DebugCounters {

static_assert(std::atomic<uint64_t>::is_always_lock_free && std::atomic<bool>::is_always_lock_free);

// Collect events only while the panel is visible. Relaxed, lock-free atomics keep updates
// cheap and safe in signal handlers. Rates come from comparing two snapshots.
enum class Counter {
	// GPU work.
	Draws,
	DrawInstances,
	IndirectDraws,
	Dispatches,
	QueueSubmits,
	GpuWaits, // CPU blocked on a GPU timeline semaphore.
	GpuWaitNs,
	GpuFaults, // Guest writes to GPU-tracked memory.
	ShadersCompiled,
	ShaderCompileNs,
	PipelinesCreated,
	PipelineCreateNs,
	// Caches.
	TextureUploadBytes,
	TextureDownloadBytes,
	TextureEvictions,
	ColorMetadataReadbacks, // CPU blocked reading back GPU-written DCC/CMASK metadata.
	ColorMetadataReadbackNs,
	ColorMetadataChecks, // GPU clear-key checks of DCC/CMASK metadata.
	BufferUploadBytes,
	BufferDownloadBytes,
	BufferEvictions,
	// Presentation.
	FlipsRequested,
	FlipsRejected, // Guest flip queue was full.
	LateVblanks,   // The present thread fell behind the vblank rate.
	SwapchainRecreations,
	DroppedPresents,
	// Emulator.
	EmulatedInstructions, // Host CPU lacked an instruction the guest used.
	UnresolvedImportCalls,
	GuestFileReadBytes,
	GuestFileWriteBytes,
	AudioUnderruns,
	// Overlapping host elapsed timings, including waits.
	GpuThreadBusyNs,
	PresentNs, // Includes image acquire and overlay preparation.
	PresentAcquireNs,
	OverlayNs,
	// Performance lab (KYTY_PERF_CSV). GPU execution time from Vulkan timestamps at the start and
	// end of each submitted command buffer; it excludes queue idle time between submissions.
	GpuExecNs,
	GpuTimedSubmits,
	PipelineBarriers,
	ImageCopies, // copyImage, blitImage, buffer<->image copies.
	BufferCopies,
	TextureCreations,
	TextureInvalidations, // Images marked CPU-dirty by guest writes.
	OcclusionDumps,          // Host-counted occlusion dumps.
	OcclusionSyntheticDumps, // Always-visible answers without --occlusion-queries.
	OcclusionPublishes,
	// CPU blocked on the GPU timeline, split by cause. Their sum equals GpuWaitNs.
	WaitFinishNs,      // Full drains: guest BufferWait, GDS reads, unmaps.
	WaitOcclusionNs,   // Occlusion query results publication.
	WaitStreamNs,      // Stream-buffer ring wrap.
	WaitReadbackNs,    // Buffer downloads and evictions.
	WaitFaultBufferNs, // GPU page-fault buffer recycling.
	WaitWriteBackNs,   // Guest access to pages with pending image write-backs.
	WaitPresentNs,     // Swapchain image reuse.
	WaitOtherNs,
	// Guest GPU thread asleep because every guest queue was suspended (WAIT_REG_MEM, predicates,
	// pending results), until new work or the retry timeout.
	CpBlockedSleeps,
	CpBlockedSleepNs,
	Count
};

enum class Peak { ShaderCompileNs, PipelineCreateNs, Count };

namespace Detail {
inline std::atomic<bool>     g_enabled {false};
inline std::atomic<bool>     g_logging {false}; // The CSV log keeps counters on with the overlay off.
inline thread_local Counter  t_wait_reason = Counter::WaitOtherNs;
inline std::atomic<uint64_t> g_counters[static_cast<int>(Counter::Count)] {};
inline std::atomic<uint64_t> g_peaks[static_cast<int>(Peak::Count)] {};
} // namespace Detail

[[nodiscard]] inline bool Enabled() noexcept {
	return Detail::g_enabled.load(std::memory_order_relaxed) ||
	       Detail::g_logging.load(std::memory_order_relaxed);
}

inline void SetLogging(bool logging) noexcept {
	Detail::g_logging.store(logging, std::memory_order_relaxed);
}

// Attributes GPU timeline waits on this thread to a cause for the scope's lifetime.
class ScopedWaitReason final {
public:
	explicit ScopedWaitReason(Counter reason) noexcept: m_previous(Detail::t_wait_reason) {
		Detail::t_wait_reason = reason;
	}
	~ScopedWaitReason() { Detail::t_wait_reason = m_previous; }
	ScopedWaitReason(const ScopedWaitReason&)            = delete;
	ScopedWaitReason& operator=(const ScopedWaitReason&) = delete;

private:
	Counter m_previous;
};

[[nodiscard]] inline Counter CurrentWaitReason() noexcept {
	return Detail::t_wait_reason;
}

#define KYTY_WAIT_REASON(reason)                                                                   \
	::Common::DebugCounters::ScopedWaitReason kyty_wait_reason_scope(                              \
	    ::Common::DebugCounters::Counter::reason)

// Stable CSV column names, in Counter order.
[[nodiscard]] const char* Name(Counter counter) noexcept;

inline void SetEnabled(bool enabled) noexcept {
	Detail::g_enabled.store(enabled, std::memory_order_relaxed);
}

inline void Add(Counter counter, uint64_t value = 1) noexcept {
	if (!Enabled()) {
		return;
	}
	Detail::g_counters[static_cast<int>(counter)].fetch_add(value, std::memory_order_relaxed);
}

class ScopedTimer final {
public:
	explicit ScopedTimer(Counter duration): ScopedTimer(Counter::Count, duration) {}
	ScopedTimer(Counter count, Counter duration, Peak peak = Peak::Count)
	    : m_count(count), m_duration(duration), m_peak(peak),
	      m_start(Enabled() ? std::chrono::steady_clock::now()
	                        : std::chrono::steady_clock::time_point {}) {}
	~ScopedTimer() {
		if (m_start == std::chrono::steady_clock::time_point {} || !Enabled()) {
			return;
		}
		const auto elapsed =
		    static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
		                              std::chrono::steady_clock::now() - m_start)
		                              .count());
		if (m_count != Counter::Count) {
			Add(m_count);
		}
		Add(m_duration, elapsed);
		if (m_peak != Peak::Count) {
			auto& peak     = Detail::g_peaks[static_cast<int>(m_peak)];
			auto  previous = peak.load(std::memory_order_relaxed);
			while (previous < elapsed &&
			       !peak.compare_exchange_weak(previous, elapsed, std::memory_order_relaxed)) {
			}
		}
	}
	ScopedTimer(const ScopedTimer&)            = delete;
	ScopedTimer& operator=(const ScopedTimer&) = delete;

private:
	Counter                               m_count;
	Counter                               m_duration;
	Peak                                  m_peak;
	std::chrono::steady_clock::time_point m_start;
};

[[nodiscard]] inline uint64_t Get(Counter counter) noexcept {
	return Detail::g_counters[static_cast<int>(counter)].load(std::memory_order_relaxed);
}

[[nodiscard]] inline uint64_t Get(Peak peak) noexcept {
	return Detail::g_peaks[static_cast<int>(peak)].load(std::memory_order_relaxed);
}

} // namespace Common::DebugCounters

#endif /* KYTY_COMMON_DEBUG_COUNTERS_H_ */
