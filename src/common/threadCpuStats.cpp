#include "common/threadCpuStats.h"

#include "common/logging/log.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace Common::ThreadCpuStats {
namespace {
bool Enabled() {
	static const bool enabled = [] {
		const auto* value = std::getenv("KYTY_THREAD_CPU_STATS");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}();
	return enabled;
}

#ifdef _WIN32
uint64_t Ticks(const FILETIME& time) {
	return (static_cast<uint64_t>(time.dwHighDateTime) << 32u) | time.dwLowDateTime;
}

uint64_t ClockNs() {
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
	                                 std::chrono::steady_clock::now().time_since_epoch())
	                                 .count());
}

struct GuestActivity {
	uint64_t              created = 0;
	std::atomic<uint64_t> yields {0}, zero_sleeps {0}, short_sleeps {0}, sleeps {0};
	std::atomic<uint64_t> requested_ns {0}, conditions {0}, condition_ns {0};
};

struct ActivityRegistry {
	std::mutex                                              mutex;
	std::unordered_map<DWORD, std::weak_ptr<GuestActivity>> threads;
};

ActivityRegistry& Activities() {
	// Guest TLS can outlive the diagnostics reporter during process shutdown.
	static auto* registry = new ActivityRegistry;
	return *registry;
}

GuestActivity* CurrentActivity() {
	thread_local auto activity = []() noexcept -> std::shared_ptr<GuestActivity> {
		try {
			auto     value = std::make_shared<GuestActivity>();
			FILETIME created {}, exited {}, kernel {}, user {};
			if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return {};
			value->created           = Ticks(created);
			auto&           registry = Activities();
			std::lock_guard lock(registry.mutex);
			registry.threads[GetCurrentThreadId()] = value;
			return value;
		} catch (...) {
			return {}; // Optional diagnostics must not make a guest syscall fail.
		}
	}();
	return activity.get();
}

struct ActivitySample {
	uint64_t created, yields, zero_sleeps, short_sleeps, sleeps, requested_ns, conditions,
	    condition_ns;
};

std::unordered_map<DWORD, ActivitySample> ReadActivities() {
	std::unordered_map<DWORD, ActivitySample> result;
	auto&                                     registry = Activities();
	std::lock_guard                           lock(registry.mutex);
	for (auto it = registry.threads.begin(); it != registry.threads.end();) {
		if (const auto activity = it->second.lock()) {
			const auto take = [](auto& counter) {
				return counter.exchange(0, std::memory_order_relaxed);
			};
			result.emplace(it->first,
			               ActivitySample {activity->created, take(activity->yields),
			                               take(activity->zero_sleeps),
			                               take(activity->short_sleeps), take(activity->sleeps),
			                               take(activity->requested_ns), take(activity->conditions),
			                               take(activity->condition_ns)});
			++it;
		} else {
			it = registry.threads.erase(it);
		}
	}
	return result;
}

struct Sample {
	uint64_t    created, cpu;
	int         priority;
	std::string name;
};
using Samples = std::unordered_map<DWORD, Sample>;

Samples ReadSamples() {
	Samples    result;
	const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	if (snapshot == INVALID_HANDLE_VALUE) return result;
	const auto    pid = GetCurrentProcessId();
	THREADENTRY32 entry {};
	entry.dwSize                      = sizeof(entry);
	using GetDescription              = HRESULT(WINAPI*)(HANDLE, PWSTR*);
	static const auto get_description = reinterpret_cast<GetDescription>(
	    GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
	if (Thread32First(snapshot, &entry)) {
		do {
			if (entry.th32OwnerProcessID != pid) continue;
			const auto thread =
			    OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ThreadID);
			if (thread == nullptr) continue;
			FILETIME created {}, exited {}, kernel {}, user {};
			if (GetThreadTimes(thread, &created, &exited, &kernel, &user)) {
				std::string name = "unnamed";
				PWSTR       wide = nullptr;
				if (get_description != nullptr && SUCCEEDED(get_description(thread, &wide)) &&
				    wide != nullptr) {
					char text[256] {};
					if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, text, sizeof(text), nullptr,
					                        nullptr) > 1)
						name = text;
					LocalFree(wide);
				}
				// Keep the one-line report intact even for a game-supplied thread name.
				std::replace(name.begin(), name.end(), '\n', ' ');
				std::replace(name.begin(), name.end(), '\r', ' ');
				result.emplace(entry.th32ThreadID,
				               Sample {Ticks(created), Ticks(kernel) + Ticks(user),
				                       GetThreadPriority(thread), std::move(name)});
			}
			CloseHandle(thread);
		} while (Thread32Next(snapshot, &entry));
	}
	CloseHandle(snapshot);
	return result;
}

struct Reporter {
	std::mutex                  mutex;
	std::condition_variable_any wake;
	std::jthread                thread;

	void Run(const std::stop_token& stop) {
		auto before = ReadSamples();
		(void)ReadActivities();
		auto             begin = std::chrono::steady_clock::now();
		std::unique_lock lock(mutex);
		while (!wake.wait_for(lock, stop, std::chrono::seconds(5), [] { return false; }) &&
		       !stop.stop_requested()) {
			auto       now        = ReadSamples();
			const auto activities = ReadActivities();
			const auto end        = std::chrono::steady_clock::now();
			const auto seconds    = std::chrono::duration<double>(end - begin).count();
			struct Row {
				DWORD         id;
				const Sample* sample;
				uint64_t      ticks;
			};
			std::vector<Row> rows;
			for (const auto& [id, sample]: now) {
				const auto old = before.find(id);
				// Ignore the first sample of a new/reused thread rather than attributing its
				// lifetime.
				if (old != before.end() && old->second.created == sample.created &&
				    sample.cpu >= old->second.cpu) {
					rows.push_back({id, &sample, sample.cpu - old->second.cpu});
				}
			}
			std::sort(rows.begin(), rows.end(),
			          [](const Row& a, const Row& b) { return a.ticks > b.ticks; });
			std::string line = fmt::format(
			    "Thread CPU {:.1f}s (100% = one logical CPU; sampled CPU time):", seconds);
			for (size_t i = 0; i < std::min<size_t>(rows.size(), 10); i++) {
				const auto& row = rows[i];
				line += fmt::format(
				    " {}#{} {:.1f}% (prio {})", row.sample->name, row.id,
				    seconds > 0 ? static_cast<double>(row.ticks) / (seconds * 100000.0) : 0,
				    row.sample->priority);
				const auto activity = activities.find(row.id);
				if (activity != activities.end() &&
				    activity->second.created == row.sample->created) {
					const auto& a = activity->second;
					line += fmt::format(
					    " [yield {:.0f}/s, sleep0 {:.0f}/s, sleep<=50us {:.0f}/s, "
					    "sleep>50us {} (requested {:.1f} ms), cond {} ({:.1f} ms wall)]",
					    a.yields / seconds, a.zero_sleeps / seconds, a.short_sleeps / seconds,
					    a.sleeps, a.requested_ns / 1e6, a.conditions, a.condition_ns / 1e6);
				}
			}
			line += '\n';
			Log::WriteToConsoleAndLog(line);
			before = std::move(now);
			begin  = end;
		}
	}
};

Reporter& GetReporter() {
	static Reporter reporter;
	return reporter;
}
#endif
} // namespace

void Start() {
#ifdef _WIN32
	if (!Enabled()) return;
	auto& reporter = GetReporter();
	if (!reporter.thread.joinable()) {
		try {
			reporter.thread = std::jthread([&reporter](std::stop_token stop) {
				try {
					reporter.Run(stop);
				} catch (...) {
					// Snapshot/format allocation failures end this optional reporter, not gameplay.
					std::fputs("Thread CPU diagnostics stopped; gameplay continues\n", stdout);
				}
			});
		} catch (const std::exception&) {
			Log::WriteToConsoleAndLog(
			    "Thread CPU diagnostics could not start; gameplay continues\n");
		}
	}
#endif
}

void Stop() {
#ifdef _WIN32
	if (!Enabled()) return;
	auto& reporter = GetReporter();
	if (reporter.thread.joinable()) {
		reporter.thread.request_stop();
		reporter.thread.join();
	}
#endif
}

void NameCurrentThread(std::string_view name) {
#ifdef _WIN32
	if (!Enabled() || name.empty()) return;
	using SetDescription              = HRESULT(WINAPI*)(HANDLE, PCWSTR);
	static const auto set_description = reinterpret_cast<SetDescription>(
	    GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetThreadDescription"));
	if (set_description == nullptr) return;
	wchar_t    text[256] {};
	const auto length = MultiByteToWideChar(
	    CP_UTF8, 0, name.data(), static_cast<int>(std::min<size_t>(name.size(), 200)), text, 255);
	if (length > 0) (void)set_description(GetCurrentThread(), text);
#else
	(void)name;
#endif
}

void RecordGuestYield() {
#ifdef _WIN32
	if (Enabled()) {
		if (auto* activity = CurrentActivity())
			activity->yields.fetch_add(1, std::memory_order_relaxed);
	}
#endif
}

void RecordGuestSleep(uint64_t nanoseconds) {
#ifdef _WIN32
	if (!Enabled()) return;
	if (auto* activity = CurrentActivity()) {
		auto& counter = nanoseconds == 0        ? activity->zero_sleeps
		                : nanoseconds <= 50'000 ? activity->short_sleeps
		                                        : activity->sleeps;
		counter.fetch_add(1, std::memory_order_relaxed);
		if (nanoseconds != 0)
			activity->requested_ns.fetch_add(nanoseconds, std::memory_order_relaxed);
	}
#else
	(void)nanoseconds;
#endif
}

GuestConditionWait::GuestConditionWait() {
#ifdef _WIN32
	if (Enabled() && CurrentActivity() != nullptr) m_begin_ns = ClockNs();
#endif
}

GuestConditionWait::~GuestConditionWait() {
#ifdef _WIN32
	if (m_begin_ns != 0) {
		if (auto* activity = CurrentActivity()) {
			activity->condition_ns.fetch_add(ClockNs() - m_begin_ns, std::memory_order_relaxed);
			activity->conditions.fetch_add(1, std::memory_order_relaxed);
		}
	}
#endif
}
} // namespace Common::ThreadCpuStats
