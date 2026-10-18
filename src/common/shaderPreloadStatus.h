#ifndef EMULATOR_SRC_COMMON_SHADERPRELOADSTATUS_H_
#define EMULATOR_SRC_COMMON_SHADERPRELOADSTATUS_H_

#include <atomic>
#include <cstdint>

// Shader preload progress (KYTY_SHADER_PRELOAD, pipelineCache.cpp): written by the guest start-up
// thread while it waits for the start-up replays, read by the system overlay, which draws the
// "Preparing shaders" screen while Active() holds, and by the window's input, which can ask to
// skip the rest of the wait (the replays then finish in the background).
namespace Common::ShaderPreload {

enum class Phase : uint32_t { Idle = 0, Shaders, Compute, RayTracing, Pipelines, Done };

struct Snapshot {
	bool     active  = false;
	Phase    phase   = Phase::Idle;
	uint64_t done    = 0;
	uint64_t total   = 0;
	// Every phase's items done and in all (the bar), and the wait's start (steady clock, ns).
	uint64_t all_done  = 0;
	uint64_t all_total = 0;
	uint64_t begin_ns  = 0;
};

namespace Detail {
inline std::atomic<bool>     g_active {false};
inline std::atomic<bool>     g_skip {false};
inline std::atomic<uint32_t> g_phase {0};
inline std::atomic<uint64_t> g_done {0}, g_total {0}, g_all_done {0}, g_all_total {0}, g_begin_ns {0};
inline std::atomic<uint64_t> g_revision {0};
} // namespace Detail

inline void Begin(uint64_t begin_ns) {
	Detail::g_skip.store(false, std::memory_order_relaxed);
	Detail::g_begin_ns.store(begin_ns, std::memory_order_relaxed);
	Detail::g_active.store(true, std::memory_order_release);
	Detail::g_revision.fetch_add(1, std::memory_order_release);
}

inline void Update(Phase phase, uint64_t done, uint64_t total, uint64_t all_done, uint64_t all_total) {
	Detail::g_phase.store(static_cast<uint32_t>(phase), std::memory_order_relaxed);
	Detail::g_done.store(done, std::memory_order_relaxed);
	Detail::g_total.store(total, std::memory_order_relaxed);
	Detail::g_all_done.store(all_done, std::memory_order_relaxed);
	Detail::g_all_total.store(all_total, std::memory_order_relaxed);
	Detail::g_revision.fetch_add(1, std::memory_order_release);
}

inline void Finish() {
	Detail::g_phase.store(static_cast<uint32_t>(Phase::Done), std::memory_order_relaxed);
	Detail::g_active.store(false, std::memory_order_release);
	Detail::g_revision.fetch_add(1, std::memory_order_release);
}

[[nodiscard]] inline bool Active() {
	return Detail::g_active.load(std::memory_order_acquire);
}

// Changes whenever the screen would change (the overlay presents a new frame then).
[[nodiscard]] inline uint64_t Revision() {
	return Detail::g_revision.load(std::memory_order_acquire);
}

[[nodiscard]] inline Snapshot Get() {
	Snapshot s;
	s.active    = Detail::g_active.load(std::memory_order_acquire);
	s.phase     = static_cast<Phase>(Detail::g_phase.load(std::memory_order_relaxed));
	s.done      = Detail::g_done.load(std::memory_order_relaxed);
	s.total     = Detail::g_total.load(std::memory_order_relaxed);
	s.all_done  = Detail::g_all_done.load(std::memory_order_relaxed);
	s.all_total = Detail::g_all_total.load(std::memory_order_relaxed);
	s.begin_ns  = Detail::g_begin_ns.load(std::memory_order_relaxed);
	return s;
}

inline void RequestSkip() {
	if (Active()) Detail::g_skip.store(true, std::memory_order_release);
}

[[nodiscard]] inline bool SkipRequested() {
	return Detail::g_skip.load(std::memory_order_acquire);
}

} // namespace Common::ShaderPreload

#endif // EMULATOR_SRC_COMMON_SHADERPRELOADSTATUS_H_
