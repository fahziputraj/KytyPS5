#pragma once

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <nlohmann/json.hpp>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace Loader::GamePatch {

enum class ClockValue { RateF32, RateF64, SecondsF32, SecondsF64, MicrosecondsU64 };

struct ClockField {
	uint64_t             address = 0;
	ClockValue           value   = ClockValue::SecondsF32;
	[[nodiscard]] size_t Size() const {
		return value == ClockValue::RateF32 || value == ClockValue::SecondsF32 ? 4 : 8;
	}
};

struct SimulationClockProfile {
	double                  nominal_fps = 60.0;
	double                  minimum_fps = 30.0;
	std::vector<ClockField> fields;
};

inline bool ParseClockAddress(const nlohmann::json& value, uint64_t& address) {
	if (!value.is_string()) {
		return false;
	}
	const auto& text   = value.get_ref<const std::string&>();
	const auto  result = std::from_chars(text.data(), text.data() + text.size(), address, 16);
	return !text.empty() && result.ec == std::errc() && result.ptr == text.data() + text.size();
}

// Only a title/version/byte-checked patch can supply these bindings. There is no
// global clock address or guessed memory layout shared by arbitrary games.
inline bool ParseSimulationClockProfile(const nlohmann::json& json, SimulationClockProfile& out) {
	SimulationClockProfile profile;
	if (json.is_string()) {
		uint64_t base = 0;
		if (!ParseClockAddress(json, base) || base > UINT64_MAX - 24) {
			return false;
		}
		// Backwards compatibility with the original Astro clock block.
		profile.fields = {{base, ClockValue::RateF64},
		                  {base + 8, ClockValue::SecondsF32},
		                  {base + 16, ClockValue::MicrosecondsU64}};
	} else {
		if (!json.is_object() || !json.contains("nominal_fps") ||
		    !json["nominal_fps"].is_number() || !json.contains("minimum_fps") ||
		    !json["minimum_fps"].is_number() || !json.contains("fields") ||
		    !json["fields"].is_array()) {
			return false;
		}
		profile.nominal_fps = json["nominal_fps"].get<double>();
		profile.minimum_fps = json["minimum_fps"].get<double>();
		for (const auto& entry: json["fields"]) {
			ClockField field;
			if (!entry.is_object() || !entry.contains("offset") ||
			    !ParseClockAddress(entry["offset"], field.address) || !entry.contains("value") ||
			    !entry["value"].is_string()) {
				return false;
			}
			const auto value = entry["value"].get<std::string>();
			if (value == "rate_f32")
				field.value = ClockValue::RateF32;
			else if (value == "rate_f64")
				field.value = ClockValue::RateF64;
			else if (value == "seconds_f32")
				field.value = ClockValue::SecondsF32;
			else if (value == "seconds_f64")
				field.value = ClockValue::SecondsF64;
			else if (value == "microseconds_u64")
				field.value = ClockValue::MicrosecondsU64;
			else
				return false;
			profile.fields.push_back(field);
			if (profile.fields.size() > 8) return false;
		}
	}
	if (!std::isfinite(profile.nominal_fps) || !std::isfinite(profile.minimum_fps) ||
	    profile.minimum_fps < 1.0 || profile.nominal_fps > 240.0 ||
	    profile.minimum_fps > profile.nominal_fps || profile.fields.empty()) {
		return false;
	}
	for (size_t i = 0; i < profile.fields.size(); ++i) {
		const auto& field = profile.fields[i];
		if (field.address % field.Size() != 0 || field.address > UINT64_MAX - field.Size()) {
			return false;
		}
		for (size_t j = 0; j < i; ++j) {
			const auto& prior = profile.fields[j];
			if (field.address < prior.address + prior.Size() &&
			    prior.address < field.address + field.Size())
				return false;
		}
	}
	out = std::move(profile);
	return true;
}

inline bool IsClockFieldGuarded(const ClockField& field, uint64_t start,
                                std::span<const uint8_t> expected) {
	return field.address >= start && field.address - start <= expected.size() &&
	       expected.size() - (field.address - start) >= field.Size() &&
	       std::any_of(expected.begin(), expected.end(), [](uint8_t byte) { return byte != 0; });
}

inline void StoreClockStep(const ClockField& field, double step) {
	void* target = reinterpret_cast<void*>(field.address);
	if (field.value == ClockValue::MicrosecondsU64) {
		const auto value = static_cast<uint64_t>(step * 1'000'000.0);
		std::memcpy(target, &value, sizeof(value));
	} else if (field.Size() == 4) {
		const auto value =
		    static_cast<float>(field.value == ClockValue::RateF32 ? 1.0 / step : step);
		std::memcpy(target, &value, sizeof(value));
	} else {
		const double value = field.value == ClockValue::RateF64 ? 1.0 / step : step;
		std::memcpy(target, &value, sizeof(value));
	}
}

} // namespace Loader::GamePatch
