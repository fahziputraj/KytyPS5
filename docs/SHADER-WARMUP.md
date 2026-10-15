# Shader and pipeline preparation preview

Local branch: `codex/shader-warmup-and-profiling`, based on U59 int19.1.

## Settings

Global settings and game-specific custom settings expose:

- **Prepare cached shaders and pipelines before starting**: blocks guest startup until the
  recorded shader permutations, graphics pipelines, compute pipelines and known software RT
  kernels finish preparation. A cancellable startup dialog shows shader and pipeline phases.
- **Reduce pipeline compilation stalls**: checks the driver cache first and uses the existing
  fast initial compilation/background optimization path when a pipeline is new.
- **Shader workers / Pipeline workers**: 0 selects up to four workers based on CPU thread count;
  manual limits are eight shader workers and sixteen pipeline workers. More is not always faster.
- **Keep warmed pipelines**: a bounded resident graphics pipeline pool, default 512, maximum 2048.
  A live draw can take an already prepared handle when the complete serialized shader, render
  state and layout signature match. This skips another driver creation call. Set 0 to disable.
  Unused handles are released before layout teardown. Driver memory use rises with the limit.
- **Pipeline list**: optional `.kyplst` recording for the same game and version. Empty also checks
  `PipelineLists/<title>.kyplst` beside the emulator. Lists hold shader locations and identities,
  not a portable Vulkan driver cache. The emulator validates their compatibility.
- **Record frame pacing and pipeline statistics**: presentation cadence (FPS, p50/p95/p99,
  longest frame, counts over 33/50/100 ms), plus compile/wait statistics, in `_Logs` beside the
  emulator. Windows also reports CPU use by named thread every five seconds; 100% means one
  logical CPU, not the entire processor. Guest rows include yield/zero-sleep/short-sleep rates
  and completed timed-condition wait durations. These counters help identify polling loops;
  a high CPU percentage alone does not prove that a thread is doing useful game work.
  Asset-loading summaries include APR and positional reads, file-mutex waits, memory-coherence
  work, native read/copy durations and the longest completed call in each window. Loading CSVs
  beside the run log work with the Tracy profiler disabled. Command-processor categories and memory-tracking costs
  help distinguish guest work, renderer work and waits. Presentation cadence excludes startup
  blank frames, system-overlay refreshes and paused/minimized periods. Foreground and background
  intervals are labelled separately and the sampling window resets on a focus change;
  it is not a measure of GPU execution time. There is small diagnostic overhead.

Settings can be inherited from the global configuration or saved per game. Rendering resolution,
effects and the game's own FPS limit are independent of these preparation controls.

## What preparation covers

AAA PC games generally ship a list of the pipelines they intend to prepare. An emulator does not
automatically know every combination that a game may request. This preview replays the current
PC's recorded history and an optional compatible pipeline list. A first run with neither has no
recorded variants to warm. Unvisited content and variants needing live guest resources can still
compile on first use. CPU emulation, texture upload and memory tracking can also cause stutters.

The startup dialog waits for `Initialized: Graphics` and does not equate an empty cache or skipped
variant with full game coverage. Shader and pipeline progress have separate totals; the compute/RT
phase is indeterminate while the driver finishes individual builds.

## Renderer flags for direct launches

The launcher configures recording, periodic cache saves, cache probes and bounded workers itself.
Command-line users may set these additional flags alongside their existing U59 preset:

```
KYTY_SHADER_PRECOMPILE_WAIT=1
KYTY_PIPELINE_JOURNAL_WAIT=1
KYTY_PIPELINE_JOURNAL_FORCE=1
KYTY_PIPELINE_PREWARM_WAIT=1
KYTY_PIPELINE_RESIDENT_MAX=512
KYTY_FRAME_STATS=1
KYTY_PIPELINE_STATS=1
KYTY_THREAD_CPU_STATS=1
KYTY_CP_GAP_STATS=1
KYTY_FAULT_COST_LOG=10
KYTY_PROFILE_LOADING=1
KYTY_LOADING_SUMMARY=1
```

`KYTY_PIPELINE_PREWARM_WAIT` waits for recorded compute and software RT jobs before guest startup.
`KYTY_PIPELINE_RESIDENT_MAX` is opt-in for direct launches (default 0; capped at 2048).
`KYTY_FRAME_STATS` is opt-in (default off). All flags leave new live-state variants to the existing
runtime fallback. Saves, game files and existing cache formats are unchanged.

For direct launches, `KYTY_LOADING_LOG` may name an absolute CSV destination. The loading publisher
adds process ID and startup timestamp suffixes to avoid overwriting earlier captures. Console
loading summaries run every five seconds with `KYTY_LOADING_SUMMARY=1`; enable
`KYTY_PROFILE_LOADING=1` too. Their completed-call wall durations are inclusive, may overlap
between threads, and include calls begun before the current window. An in-flight call has no
completed duration yet. They are not exclusive CPU timings or frame-time percentages. Thread
activity is reported for the ten threads with the highest sampled CPU use; exited/new threads
may not appear in that interval. Diagnostic counters add overhead and can be switched off.

## Guest sleep timing correction

Guest sleeps longer than the 10 ms signal-poll slice now measure elapsed time from one monotonic
origin. Previously, each timer overshoot and signal-dispatch cost was added to the full requested
sleep, so long sleeps drifted further as they were split into more slices. The remaining timeout
now includes that elapsed work. Zero sleeps and the existing short-sleep/yield path are preserved;
the polling interval, guest priorities, CPU placement and vblank frequency are unchanged.

This corrects timeout accounting. Its effect on Astro Bot's frame rate has not been measured;
it does not by itself establish the cause of continuous stutter.

## Asset staging reuse

APR asset reads reuse a private host buffer per reading thread, capped at the existing 4 MiB
chunk size. Previously each read allocated and zero-filled a fresh vector. The buffer grows only
when a larger chunk is needed; only the bytes returned by the file read are copied to guest
memory. Read offsets, chunk sizes, short-read handling, guest-copy order and memory fault handling
are preserved. A nested read gets its own temporary buffer while the outer read is using the
cached buffer. Each thread releases its retained storage on exit.

`KYTY_APR_STAGING_REUSE=0` restores allocation of a fresh zero-filled buffer for each read. Reuse
is enabled by default in this preview, independent of diagnostics. It removes redundant host
allocation and clearing work; the change has not been measured in foreground Astro Bot gameplay
and does not eliminate file I/O, guest memory faults or shader compilation.

For investigation, keep the game in front, use the same level and camera position, and distinguish
the initial load from settled gameplay. Startup blank frames, menus, background runs and different
camera directions are not interchangeable samples. These diagnostics do not by themselves improve
the game frame rate. CPU time samples also cannot distinguish useful guest computation from a busy
polling loop; a hot thread needs further inspection before choosing a scheduler or code change.

## Build

The local preview uses LLVM 22.1.3, Qt 6.10.3, Release, IPO and the bundled Astro Bot PGO profile,
with source mapped to `C:\kyty-src19` as described in EXPERIMENTAL.md. On machines with an old
Windows SDK, `-DTRACY_NO_SYSTEM_TRACING=ON` disables optional OS trace sampling. This does not
disable the frame pacing or pipeline summaries above.

This is a development preview. Build completion does not establish an FPS improvement or prove
stutter-free gameplay. Compare the same scene, build, settings and warmed cache before claiming
a performance change.
