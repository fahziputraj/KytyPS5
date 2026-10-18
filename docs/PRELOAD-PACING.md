# Shader preload and the pace maintainer

Both are on by default (code defaults and `u59-preset.json`) and work for every game. Set the
variables below to `0` to turn a part off.

## Shader preload (`KYTY_SHADER_PRELOAD`)

Native PC games build their shaders and pipeline state objects before the main menu, so play
never waits for the driver. KytyPS5 now does the same with what earlier runs of the same game
recorded (`_PipelineCache/<title>.shaders.journal` and `<title>.pipelines.journal`, written by
`KYTY_SHADER_PRECOMPILE` and `KYTY_PIPELINE_JOURNAL`):

1. The game's first thread waits before the game's entry point. The window stays responsive and
   shows a **Preparing shaders** panel (over the game's splash) with the phase, the count and an
   overall bar.
2. Every journaled shader permutation is compiled (no 30000 cap), then the compute and ray-tracing
   pipelines it needs, then every journaled graphics pipeline, on `KYTY_SHADER_PRELOAD_THREADS`
   workers (default: logical processors - 2, at most 16; 16 on an i5-13500) at normal priority
   instead of 2-4 low-priority background threads.
3. The graphics pipelines built are **kept** (`KYTY_PIPELINE_RESIDENT_MAX`, default 4096; the boot
   level's pipelines first). The draw that first needs one takes it without any driver call. A
   pipeline the driver cache already holds is built from it (quick) so that it can be kept too.
4. The game starts. Esc, Enter or a pad's Options/Cross button starts it at once; the remaining
   work then finishes in the background. `KYTY_SHADER_PRELOAD_MAX_S=N` starts it after N seconds.

The first run of a game has nothing recorded and starts at once; it records as it plays. The
next start prepares everything that run met. After a game or emulator update the shaders are
translated again (the journal stores inputs, not outputs), so that start takes longer once.
Later starts reload the translated programs (`KYTY_PROGRAM_CACHE`) and the driver cache, and take
seconds. The log shows `Shader preload:` lines with counts and times, and at exit how many kept
pipelines draws took.

Content never visited in an earlier run, and variants that depend on live guest resources, still
compile on first use (fast-first keeps that short). Preloading removes compile hitches; it does
not change the cost of rendering a scene.

### Measured (2026-10-18)

ASTRO BOT PPSA21567 01.019, i5-13500 + RTX 5070 Ti, the same 150 s from boot without input,
`KYTY_PIPELINE_STATS=1` and `KYTY_PACE_LOG=1`. The second launch had journals from an older build
whose modules no longer matched (nothing to preload); the third had the second launch's journals.

| 10 s windows | 2nd launch | 3rd launch (preloaded) |
| --- | --- | --- |
| Graphics pipelines taken from the preload | 0 | all (279 kept, 0 driver calls at draw) |
| Compile wait of the command processor | 430-5950 ms | 0.1-1.1 ms |
| Flips per second | 18.7-59.7 | 59.2-60.0 |

The preload finished during start-up (about 0.2 s for the pipelines with a warm driver cache),
so the game was not delayed. Content the earlier run never reached still compiled on first use.

## Pace maintainer (`KYTY_PACE`, `KYTY_PACE_VRR`)

The present thread runs the guest's vblank clock and presents the game's flips.

- `KYTY_PACE=1`: guest vblanks follow absolute deadlines from one origin, waited for with the
  high-resolution timer, so timer overshoot and presentation time never slow the guest clock. A
  vblank a host stall delayed is still delivered with its count (time measured in vblanks keeps up
  with real time), at most 8 at once; beyond that the count jumps with a single event.
- `KYTY_PACE_VRR=1`: a finished frame is presented as soon as its flip interval (the game's flip
  rate: 60, 30 or 20 Hz) since the previous flip has passed, instead of at the next vblank tick.

The second point targets the worst slow motion. On the console, and in the old loop, a frame
that misses a vblank waits for the next one. A game that waits for its previous flip before it
renders the next frame (double buffering) and whose frame takes 17 ms can then run at 30 FPS
instead of about 58, and a game that advances its simulation once per frame (many console games,
Astro Bot among them) then at half speed. Presented when ready, the same game can run at about
58 FPS and 98% speed. How much a given game gains depends on how it waits for its flips. The flip interval stays a minimum on average, so no game runs
faster than its flip rate allows.

With a G-SYNC/FreeSync display each frame is shown when it is done. Keep the present mode at
Mailbox (the default) or Immediate. On NVIDIA, "Vulkan/OpenGL present method: Prefer layered on
DXGI Swapchain" in the NVIDIA Control Panel lets G-SYNC work in a window.

`KYTY_PACE_LOG=1` prints a `Pace 10s:` line: flips per second, how many went out between vblanks
and at vblank ticks, the longest gap between flips, and late or coalesced vblanks.

What it cannot do: a game that steps its simulation once per frame still slows down when the
emulator delivers fewer frames than the game's target rate (for example 45 FPS against 60). No
emulator-side clock can fix that without the game's cooperation; it needs a faster frame or a
title-specific clock patch.

## Scheduling (`KYTY_HYBRID_PLACEMENT`, `KYTY_ECOQOS`)

- Windows 11 puts a background or covered window's process on EcoQoS (E-cores, lower clocks) and
  ignores its 1 ms timer resolution, which stretches the guest's timed waits to 15.6 ms. The
  emulator now opts the whole process out (`KYTY_ECOQOS=1` keeps the Windows behaviour).
- `KYTY_HYBRID_PLACEMENT=auto` (default): on Intel hybrid processors the background threads
  (shader and pipeline compiles during play, cache savers, monitors) run on the E-cores, keeping
  the P-cores for the game, the command processor and the renderer, and the command processor's
  reserved core (`KYTY_CPU_RESERVE=cp`) is a P-core. During the preload the compile workers use
  every core. `0` turns it off.

## NVIDIA Smooth Motion

Jetsku/KytyPS5#58 is merged: CUDA is initialized before the guest address space, so NVIDIA
Smooth Motion (RTX 50 driver frame generation, enabled per program in the NVIDIA App) can start
for `kyty_emulator.exe`. It doubles the presented frames; it does not change the game's own speed.
`KYTY_CUDA_EARLY_INIT=0` turns the early initialization off.

## Fewer first-time freezes: parallel pipeline prefetch

A pipeline no earlier run journaled is still compiled on first use. Before, with fast-first on
(NVIDIA), each one was built on the command processor, one after another: a burst of 32 new
pipelines froze Astro Bot for about 6 s. Now `KYTY_PIPELINE_PREFETCH` works together with
fast-first: draw preparation predicts the pipelines of upcoming draws and
`KYTY_PIPELINE_PREFETCH_THREADS` workers (default 6) build their unoptimized versions in
parallel (or take a preloaded one); the optimized builds still follow in the background, and
these pipelines are journaled for the next preload as before.

## Native upscaler: FSR 1 (`KYTY_PRESENT_UPSCALER`)

A game frame smaller than the window (Astro Bot outputs 1080p below a 4K window) is upscaled with
AMD FidelityFX Super Resolution 1: EASU edge-adaptive upsampling, then RCAS sharpening
(`KYTY_PRESENT_FSR_SHARPNESS`, stops, 0 = sharpest, default 0.25). `KYTY_PRESENT_UPSCALER=bilinear`
keeps the old blit. HDR frames keep the blit. To use it on a 1440p monitor set the launcher's
resolution to 2560x1440 or use fullscreen. The FSR 1 shader headers are MIT licensed
(`src/graphics/host_gpu/shaders/ffx`, `LICENSES/FidelityFX-SDK-MIT.txt`).

## Anti-aliasing: 4K supersampling (instead of DLAA / FSR Native AA)

DLAA and FSR Native AA are temporal: they need the game's per-pixel motion vectors, depth and
camera jitter, which an emulator cannot obtain in general. The anti-aliasing that does work is
supersampling: launcher resolution **3840x2160** with **Fullscreen** on. The emulator then reports
a 4K output, games such as ASTRO BOT render 4K frames, and the presenter shrinks them to the
screen with a box filter (`KYTY_PRESENT_BOX_DOWNSCALE=1`, now in the preset; it averages the
pixels instead of the aliasing bilinear blit).

Measured on ASTRO BOT, RTX 5070 Ti, 2560x1440 screen: 37-60 flips/s with 4K output against 32-56
with 1080p output on the same route; GPU load stayed around 40-50%. The frame rate is set by the
CPU side of the emulator, so the extra GPU work is nearly free on this PC. With 1080p output in
fullscreen the frame is upscaled with FSR 1 instead.

## Frame generation: NVIDIA Smooth Motion

Game-integrated frame generation (FSR 3, DLSS FG) needs the game's motion vectors, which an
emulator cannot obtain in general. NVIDIA Smooth Motion (RTX 50 driver frame generation, NVIDIA
App > Graphics > Program Settings > `kyty_emulator.exe`) can start for the emulator, which
initializes CUDA early for it (`CUDA: initialized ... (cuInit 0)` in the log). Its driver layer
logs to `C:/NvPresent/logs` when it runs for a program.

On 2026-10-18 ASTRO BOT (PPSA21567) hung on a black boot screen (60 FPS, intro video never
started) in every run for about 40 minutes, including the older int19.1 build and runs without a
save or caches; before and after that the same setups booted into play, also with Smooth Motion
on. The cause was not found. If it happens, close and restart the emulator.

## Accuracy

The launcher's accurate options are the ones to keep for the most faithful image: accurate GPU
occlusion, linear-image readback, raw-image buffer sync and tessellation (all on in the current
`Kyty.ini`). The fork's other speedups are verified to give identical results. Remaining
differences from the console are emulator bugs of individual games, not settings.
