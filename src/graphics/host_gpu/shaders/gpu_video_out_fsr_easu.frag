#version 450
#extension GL_GOOGLE_include_directive : require

// KYTY_PRESENT_UPSCALER=fsr (PresentUpscaler, swapchain.cpp): AMD FidelityFX Super Resolution 1
// edge-adaptive spatial upsampling (EASU) of the video-out frame to the window size. The frame is
// display-referred (sRGB or PQ encoded), as EASU expects. Output: the intermediate image RCAS reads.

#define FFX_GPU             1
#define FFX_GLSL            1
#define FFX_HALF            0
#define FFX_FSR_EASU_FLOAT  1
#include "ffx/ffx_core.h"

layout(set = 0, binding = 0) uniform sampler2D source;

layout(push_constant) uniform Constants {
	uvec4 con0;
	uvec4 con1;
	uvec4 con2;
	uvec4 con3;
} constants;

FfxFloat32x4 FsrEasuRF(FfxFloat32x2 p) {
	return textureGather(source, p, 0);
}
FfxFloat32x4 FsrEasuGF(FfxFloat32x2 p) {
	return textureGather(source, p, 1);
}
FfxFloat32x4 FsrEasuBF(FfxFloat32x2 p) {
	return textureGather(source, p, 2);
}

#include "ffx/fsr1/ffx_fsr1.h"

layout(location = 0) out vec4 out_color;

void main() {
	FfxFloat32x3 color;
	ffxFsrEasuFloat(color, FfxUInt32x2(gl_FragCoord.xy), constants.con0, constants.con1, constants.con2,
	                constants.con3);
	out_color = vec4(color, 1.0);
}
