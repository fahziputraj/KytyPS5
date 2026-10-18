#ifndef EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SHADERPRELOAD_H_
#define EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SHADERPRELOAD_H_

namespace Libs::Graphics {

// KYTY_SHADER_PRELOAD (pipelineCache.cpp): returns once the renderer's start-up replays of the
// shaders and pipelines journaled by earlier runs are done, or skipped; at once when there is
// nothing to preload, and on every later call. The guest's first thread calls it before the game's
// entry point, while the window shows the progress (common/shaderPreloadStatus.h).
void WaitForShaderPreload();

} // namespace Libs::Graphics

#endif // EMULATOR_SRC_GRAPHICS_HOST_GPU_RENDERER_PIPELINE_SHADERPRELOAD_H_
