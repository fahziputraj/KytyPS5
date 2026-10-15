#pragma once

#include <cstdint>
#include <string_view>

namespace Common::ThreadCpuStats {
// Opt-in Windows diagnostics (KYTY_THREAD_CPU_STATS=1); no scheduler or priority changes.
void Start();
void Stop();
void NameCurrentThread(std::string_view name);

// Counters only: no extra yielding, sleeping or changes to guest scheduling.
void RecordGuestYield();
void RecordGuestSleep(uint64_t nanoseconds);

class GuestConditionWait final {
public:
	GuestConditionWait();
	~GuestConditionWait();
	GuestConditionWait(const GuestConditionWait&)            = delete;
	GuestConditionWait& operator=(const GuestConditionWait&) = delete;

private:
	uint64_t m_begin_ns = 0;
};
} // namespace Common::ThreadCpuStats
