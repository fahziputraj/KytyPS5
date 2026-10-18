#version 450
#extension GL_GOOGLE_include_directive : require

// KYTY_PRESENT_UPSCALER=fsr (PresentUpscaler, swapchain.cpp): AMD FidelityFX Super Resolution 1
// robust contrast-adaptive sharpening (RCAS) of the EASU output, written to the swapchain image.
// constants.con: FsrRcasCon's sharpness (KYTY_PRESENT_FSR_SHARPNESS); constants.size: the extent.

#define FFX_GPU     1
#define FFX_GLSL    1
#define FFX_HALF    0
#define FSR_RCAS_F  1
#include "ffx/ffx_core.h"

layout(set = 0, binding = 0) uniform sampler2D source;

layout(push_constant) uniform Constants {
	uvec4 con;
	ivec2 size;
} constants;

FfxFloat32x4 FsrRcasLoadF(FfxInt32x2 p) {
	return texelFetch(source, clamp(p, ivec2(0), constants.size - ivec2(1)), 0);
}
void FsrRcasInputF(inout FfxFloat32 r, inout FfxFloat32 g, inout FfxFloat32 b) {}

#include "ffx/fsr1/ffx_fsr1.h"

layout(location = 0) out vec4 out_color;

void main() {
	FfxFloat32 r;
	FfxFloat32 g;
	FfxFloat32 b;
	FsrRcasF(r, g, b, FfxUInt32x2(gl_FragCoord.xy), constants.con);
	out_color = vec4(r, g, b, 1.0);
}
