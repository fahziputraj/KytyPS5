#ifndef KYTY_LOADER_GAME_PATCH_H_
#define KYTY_LOADER_GAME_PATCH_H_

#include "common/common.h"

#include <filesystem>
#include <vector>

namespace Loader {

struct Program;

namespace GamePatch {

bool Apply(const std::filesystem::path& plan_path, Program* main_program,
           const std::vector<Program*>& programs);
bool ApplyPending(Program* program);
void Clear();
// A timing patch's paired presentation limit (its "presentation_fps"; 0: none).
[[nodiscard]] uint32_t PresentationFpsLimit();
// A main-output flip was presented at `now` (performance counter, `frequency` ticks per second):
// a patch's simulation clock (simulationClockProfile.h) updates the title's timestep.
void OnPresentedFrame(uint64_t now, uint64_t frequency);

} // namespace GamePatch

} // namespace Loader

#endif // KYTY_LOADER_GAME_PATCH_H_
