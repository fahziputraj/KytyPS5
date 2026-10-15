#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>

namespace Common {

// Presentation cadence, not GPU execution time. Called by the serialized presenter only.
// No allocation or sorting on the per-frame path; the bounded window is sorted on reporting.
class FramePacingStats {
public:
	struct Report {
		double seconds = 0, fps = 0, p50_ms = 0, p95_ms = 0, p99_ms = 0, worst_ms = 0;
		uint32_t frames = 0, over_33_ms = 0, over_50_ms = 0, over_100_ms = 0;
	};

	void Reset() { m_started = false; m_count = 0; }

	[[nodiscard]] std::optional<Report> Present(uint64_t now_ns) {
		if (!m_started || now_ns <= m_previous_ns) {
			m_started = true;
			m_previous_ns = now_ns;
			m_count = 0;
			m_sum_ns = 0;
			return {};
		}
		const auto interval = now_ns - m_previous_ns;
		m_previous_ns = now_ns;
		m_intervals[m_count++] = interval;
		m_sum_ns += interval;
		if (m_sum_ns < 10'000'000'000ull && m_count < m_intervals.size()) return {};

		std::sort(m_intervals.begin(), m_intervals.begin() + m_count);
		const auto percentile = [this](uint32_t percent) {
			const auto rank = (m_count * percent + 99u) / 100u;
			return static_cast<double>(m_intervals[rank - 1]) / 1e6;
		};
		Report report;
		report.seconds = static_cast<double>(m_sum_ns) / 1e9;
		report.frames = m_count;
		report.fps = static_cast<double>(m_count) / report.seconds;
		report.p50_ms = percentile(50);
		report.p95_ms = percentile(95);
		report.p99_ms = percentile(99);
		report.worst_ms = static_cast<double>(m_intervals[m_count - 1]) / 1e6;
		for (uint32_t i = 0; i < m_count; i++) {
			report.over_33_ms += m_intervals[i] > 33'333'334;
			report.over_50_ms += m_intervals[i] > 50'000'000;
			report.over_100_ms += m_intervals[i] > 100'000'000;
		}
		m_count = 0;
		m_sum_ns = 0;
		return report;
	}

private:
	std::array<uint64_t, 4096> m_intervals {};
	uint64_t m_previous_ns = 0, m_sum_ns = 0;
	uint32_t m_count = 0;
	bool m_started = false;
};
} // namespace Common
