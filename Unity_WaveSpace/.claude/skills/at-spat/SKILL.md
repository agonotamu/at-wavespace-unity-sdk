---
name: at-spat
description: >
  Reference knowledge for AT WaveSpace (AT SPAT) — Antoine Gonot's JUCE-based C++ spatial audio engine with
  Unity C# integration. Use this skill whenever the conversation involves AT SPAT, the
  spatialization engine, WFS, binaural virtualization, HRTF, At_Player, At_MasterOutput,
  At_SpatializationEngine, AT_Spatializer, corpus generation, EvaluationAppConsole, C2 mode,
  audio glitches, fade transitions, Unity DLL integration, or any file named with the At_/AT_
  prefix. Also trigger for questions about the C# state system, the JSON state files, or
  the Unity Inspector editors for this engine.
---

# AT SPAT — Engine Reference

## Overview

AT SPAT is a real-time spatial audio engine built by Antoine Gonot (Art-Tone).

- **C++ library**: JUCE-based, exposes a C interface (`extern "C"`) (`AT_WS_*` exports, WS = WaveSpace) in `at_wavespace_engine.dll` / `.dylib`
- **Unity integration**: C# MonoBehaviour wrappers calling the DLL via P/Invoke
- **Spatialization modes**: WFS (Wave Field Synthesis), Binaural via HRTF, Simple Binaural (A/B test, bypasses WFS)
- **Audio backends**: ASIO / WASAPI (Windows), CoreAudio (macOS)

---

## C++ Library Architecture

### Key files

| File | Role |
|---|---|
| `PluginInterface.h/.cpp` | Top-level C interface (`AT_WS_*` exports, `__stdcall` on Windows) |
| `AT_AudioManager.h/.cpp` | Owns the `SpatializationEngine`, device enumeration/cache |
| `AT_SpatializationEngine.h/.cpp` | Owns `AudioDeviceManager`, orchestrates all players (`getNextAudioBlock`) |
| `AT_SpatPlayer.h/.cpp` | Per-source audio processing (file reading, fade, per-sample WFS loop) |
| `AT_Spatializer.h/.cpp` | Wave Field Synthesis delay/gain/speaker-mask computation (class `AT::Spatializer`) |
| `AT_BinauralSimpleSpatializer.h/.cpp` | Simple binaural mode (direct HRTF, no WFS) |
| `HRTFProcessor.h/.cpp`, `HRTFTable.h/.cpp` | FFT/OLA HRTF convolution, shared IR table |
| `AT_WfsPrefilter.h`, `AT_NearFieldCorrection.h` | WFS half-derivative IIR prefilter, NFC shelving filter |
| `AT_SixDofMaskProcessor.h/.cpp` | 6DOF source masking (2D players) |
| `UnityLogger.h/.cpp`, `AT_SpatConfig.h` | `LOG` macros (stripped by `DISABLE_UNITY_LOGGING`), build switches |

### JUCE console app specifics
- Use `ScopedJuceInitialiser_GUI` as **first variable in `main()`**
- **Never use async device scan** (no message loop in console apps) — always `refreshDevices(false, false)` (sync)
- Macro to control this: `AT_SPAT_CONSOLE_APP` defined via CMake or Projucer preprocessor definitions

### Critical constants
```cpp
static constexpr int MAX_VIRTUAL_SPEAKERS = 1024;  // AT_Spatializer.h — must match C# side
```
Pre-allocated arrays sized to `MAX_VIRTUAL_SPEAKERS` in both C++ (`SpatializationEngine`) and C# (`At_MasterOutput`, `At_Player`) to avoid per-frame GC/heap allocation.

### Audio thread model
- `SpatializationEngine` owns `AudioDeviceManager`
- `getNextAudioBlock()` runs on audio thread — no allocation, no locks
- Output fade: `juce::LinearSmoothedValue<float> m_fadeGain` applied at output stage (not input)
- Double buffer pattern for speaker positions: audio thread reads from committed snapshot, main thread writes with atomic flag

### Mode transitions (WFS ↔ Simple Binaural)
- Controlled by `AT_WS_setIsSimpleBinauralSpat(bool)`
- `isPrevSimpleBinauralSpat` tracks last sent value to avoid redundant calls
- Crossfade with warmup blocks to avoid clicks

### Known open issues (as of early 2026 — see "Updates" section at the end for current state)
- Freeze/glitches when moving listeners or virtual speakers in Unity (position update timing)
- WFS delay jitter in wave shader visualization
- Missing normalization in Simple Binaural mode

### Fixed bugs (for context)
- `m_fadeGain=0` silence after `stopWithFade()` → fixed fade state reset in `start()`
- Binaural buffer overflow → fixed buffer sizing
- Race conditions in `At_Player` enable/disable
- ASIO crashes on shutdown

---

## Corpus Generation (EvaluationAppConsole)

Python + C++ console application for generating evaluation corpora.

| Mode | Description |
|---|---|
| **C1** | Records via physical KEMAR microphones from real WFS speakers |
| **C2** | Records binaural output directly from the library (post-HRTF processing) |

**C2 issue (as of early 2026)**: silent output files — root cause is wrong writer tap placement (recording before HRTF processing rather than after). Fix requires tapping the output after `AT_BinauralSimpleSpatializer::processBlock()`.

---

## C# Unity Integration

### Scene hierarchy
```
At_MasterOutput (MonoBehaviour)
└── VirtualSpeakers (GameObject parent)
    ├── spk0 (At_VirtualSpeaker)
    ├── spk1 (At_VirtualSpeaker)
    └── ...
At_Listener (MonoBehaviour)
At_Player × N (MonoBehaviour)
```

### Key MonoBehaviours

**`At_MasterOutput`**
- Owns engine lifecycle: `AT_WS_initialize()` → `AT_WS_setup()` → `AT_WS_shutdown()`
- Registers log callback `OnSpatEngineLog` in `Awake()`
- Sends speaker transforms each frame via pre-allocated arrays (`m_speakerPositions/Rotations/Forwards`, indexed by `speaker.id * 3`)
- `MAX_VIRTUAL_SPEAKERS = 1024` — must match C++ constant
- `addPlayer(At_Player p)` → calls `AT_WS_addPlayer(out uid, is3D, isLooping)`, assigns `p.spatID`

**`At_Player`**
- Pre-allocated WFS arrays: `delayArray`, `volumeArray`, `activationSpeakerVolume` (all `float[MAX_VIRTUAL_SPEAKERS]`)
- `initPlayer()` loads from JSON state, sets file path, optionally auto-plays
- `UpdateSpatialParameters()`: queries WFS params from engine, updates `At_VirtualSpeaker.isActive`
- GUID-based identity: `setGuid()` on Reset/Duplicate/DragPerform

**`At_Listener`**
- Sends position/rotation/forward each `Update()` via `AT_WS_setListenerTransform()`
- Handles gimbal-lock edge case: when `eulerY==180 && eulerZ==180` → normalize

**`At_VirtualSpeaker`**
- `id`: index used by engine (maps to `speaker.id * 3` in position arrays)
- `isActive`: set by `At_Player.UpdateSpatialParameters()` based on speaker mask
- `m_maxDistanceForDelay`: set by `At_MasterOutput` from `outputState.maxDistanceForDelay`

### State system (persistence)

State is serialized to `StreamingAssets/{sceneName}_States.txt` (newline-delimited JSON):
- Line 0: `At_OutputState` (device, channels, gain, HRTF path, speaker config…)
- Lines 1…N: `At_PlayerState` per player (guid, fileName, gain, is3D, filters…)

Virtual speaker layout: `StreamingAssets/{sceneName}_VirtualSpeakerState.txt`

`At_AudioEngineUtils` (static class):
- Loaded once via static constructor on first access
- `SaveAllState(sceneName)` — writes both output and player states
- `CleanAllStates(sceneName)` — removes states for GUIDs no longer in scene
- `getOutputState()` / `getPlayerStateWithGuidAndName()` — main accessors

### Editor scripts

**`At_MasterOutputEditor`**
- Device enumeration: `AT_WS_getCachedDeviceInfo()` after `AT_WS_waitForDeviceScan(10000)`
- Device list cached with 5s cooldown (`DEVICE_REFRESH_COOLDOWN`)
- Speaker config regeneration: destroys existing `At_VirtualSpeaker` GameObjects, calls `At_SpeakerConfig.addSpeakerConfigToScene()`

**`At_PlayerEditor`**
- File metadata read in Edit mode via `AT_WS_getAudioFileMetadata()` (reads headers only, fast)
- Inspector repaints throttled: `RequiresConstantRepaint()` returns true only when playing
- `UpdatePlayerParameters()` throttled to 10/s when not playing

**`At_MixerWindow`** (`AT_3DAudioEngine/Mixer` menu)
- Compact strip view: one column per player + master
- Repaints at ≤10 fps (`REPAINT_INTERVAL = 0.1`)

### Speaker configurations (`At_SpeakerConfig`)

| `outputConfigDimension` | Layout | Constraint |
|---|---|---|
| 1 | Linear (1D, along X) | Any count |
| 2 | Square perimeter (2D) | Count % 4 == 0 |
| 3 | Square 3/4 (2D) | Count % 3 == 0 |
| 4 | Full circle (2D) | Any count |
| 5 | Half circle (2D) | Any count |

### Native API (DLL exports, selection)

```csharp
AT_WS_initialize() → int
AT_WS_shutdown()   → int
AT_WS_setup(deviceName, inputCh, outputCh, bufferSize, isBinaural) → int
AT_WS_addPlayer(out uid, is3D, isLooping) → int
AT_WS_removePlayer(uid) → int
AT_WS_startPlayer(uid) / AT_WS_stopPlayer(uid) → int
AT_WS_setPlayerTransform(uid, pos[], rot[], fwd[]) → int
AT_WS_setPlayerRealTimeParameter(uid, gain, speed, atten, minDist) → int
AT_WS_setPlayerFilePath(uid, path) → int
AT_WS_setListenerTransform(pos[], rot[], fwd[]) → int
AT_WS_setVirtualSpeakerTransform(pos[], rot[], fwd[], count) → int
AT_WS_setMasterGain(gain) / AT_WS_setMakeupMasterGain(gain) → int
AT_WS_loadHRTF(filePath) / AT_WS_loadDefaultHRTF() → int
AT_WS_setIsSimpleBinauralSpat(bool) → int
// Binaural virtualization is chosen at AT_WS_setup() only — there is no AT_WS_setIsBinauralVirtualization export
AT_WS_getMixerOutputMeters(ptr, size) → int
AT_WS_getPlayerMeters(uid, ptr, size) → int
AT_WS_getPlayerWfsDelay(uid, ptr, size) → int
AT_WS_getPlayerWfsLinGain(uid, ptr, size) → int
AT_WS_getPlayerSpeakerMask(uid, ptr, size) → int
AT_WS_getAudioFileMetadata(path, out ch, out sr, out len, out samples) → int
AT_WS_setLogCallback(callback) → void
// Device enumeration (Editor only)
AT_WS_getDeviceCount() → int
AT_WS_getCachedDeviceInfo(i, nameBuf, typeBuf, ref maxIn, ref maxOut) → int
AT_WS_getDetailedDeviceInfo(...) → int
AT_WS_waitForDeviceScan(timeoutMs) → int
AT_WS_filterUnavailableDevices() → void
```

Return codes: `0 = AUDIO_PLUGIN_OK`, `1 = AUDIO_PLUGIN_ERROR`

Gains are in **dB** despite the "linear" wording in `PluginInterface.h`: `AT_WS_setMasterGain`,
`AT_WS_setMakeupMasterGain` and the `gain` of `AT_WS_setPlayerRealTimeParameter` go through `pow(10, g/20)`.

### Git / local sync scripts
6 shell scripts for syncing between local project directories and GitHub:
`UpdateLibFromGit`, `UpdateGitFromLib`, and variants.

---

## Pending features (TODO)
- Per-player low-pass / high-pass filters (DLL imports and UI present, native processing not yet wired)
- C2 corpus mode fix (post-HRTF tap)

---

## Updates Feb → Oct 2026 (project name now **AT WaveSpace** / AT_WaveSpace; aliases AT SPAT)

### Build
- Migrated from Projucer to **CMake** (`CMakePresets.json`), cross-platform: macOS universal binary
  (arm64 + x86_64), Windows x64, Android / Oculus Quest 2.
- Windows pitfall seen: build launched before configure → always `cmake --preset …` before `cmake --build`.
- macOS Tahoe code-signing regression fixed via a Unity `AssetPostprocessor`.
- MIT licence; GitHub README finalized.

### WFS evaluation geometry (fixed, reference)
- 32 HP × 18.5 cm, array length 5.735 m, at Z = 0.
- Listener Z = −3 m or −4.39 m; source Z = listener_Z + dist (dist = KEMAR→source).
- Source always **between listener and array** → listener always in the divergent zone (valid WFS).
- Example: dist 20 cm with listener at −3 m → source 2.80 m from array.
- Spatial aliasing ≈ 919 Hz; NFC reference distance rRef = 3.0 m.

### DSP features added
- **NFC**: rigid-sphere model, zero/pole correction, compile-time disable flag, stale-state bug fixed.
- **DVF**, WFS amplitude regularization (`secondarySourceSize` ε, propositions P1/P2/P3).
- **Speaker-mask normalization smoother.** Note: mask normalization produced a 23.44 Hz block-rate
  modulation (rotation/translation artifacts) — watch any per-block recomputation.
- Simple Binaural A/B mode; amplitude panning / binaural dropdown in Unity.
- **AT_SpatPlayer playlist**: N files, modes Single / Random / Shuffle.
- Multithreaded HRTF: `AbstractFifo` worker queue, IR reload throttling (MAX = 8); HRTF OLA click fixes.
- **6DOF navigation** (FocalSources), `AT_SixDofMaskProcessor`.
- 1024-MEMS sphere microphone array integration (LMA/PRISM): GCC-PHAT (chosen over MUSIC for real time),
  mode-based multi-source detection, Tikhonov ε = 1e-6 for spherical interpolation;
  geometry convention: angle origin at −π/2, x = R·sin, z = R·cos.
  Python tools: `sphere_subsample.py` (FPS / two-ring / planar), `h5_analyse.py`.
- HRTF data: QU KEMAR (TU Berlin, Zenodo DOI 10.5281/zenodo.55418), SOFA → TXT conversion.

### Concurrency bugs already fixed (do not reintroduce)
- `prepare()` race; SpinLock contention on speaker positions; spinlock race on speaker positions.
- **Windows multithreading (11 bugs)**: thread priority inversion, lambda captures with dangling
  references, data races, `shared_mutex` destroyed while in use.
- Startup clicks; audio device init; JSON serialization attributes (C#).
- **Oct 2026**: players writing engine-wide smoothers from worker threads; counter-based threading
  timeout path; reference capture of the callback's buffer info; source swap racing the audio thread
  in `SpatPlayer` (details in the performance section below).

### Unity
- Demo `WFS_WarehouseScene`: 5 WFS sources, VFX particles, speaker halo lighting, FPS controller.
- SteamVR work (Oct 2026): **Unity 2022.3 LTS** chosen (OpenVR package problems in Unity 6.4);
  AT_WaveSpace menu window to place virtual speakers by drawing a segment with an HTC Vive (1st gen)
  controller.
- The Unity Editor locks the loaded native DLL: close Unity (or copy the DLL afterwards) before
  rebuilding the library.

### Windows performance investigation (12 Oct 2026) — resolved
Symptom: clicks from 8–10 sources at 32 channels, larger buffer did not help, M4 held more.
Machine: Dell Precision 7560, i9-11950H (8 cores / 16 threads, not hybrid). Measured with the bench in
stereo downmix on the Realtek output (no 32-channel interface available). Kit and protocol:
`tools/perf/PERF.md`; state of the instrumentation: `tools/perf/engine/PATCH.md`.

Causes found and fixed (listening limit in `TestClaude_Orbit`, 32 virtual speakers, HRTF downmix:
≈10 sources before → ≈18–19 after fixes 1+2 → ≈100 after fix 3):
1. **Denormals** (x86 only — the best explanation for the gap with the M4): there was no
   `ScopedNoDenormals` anywhere. A source decaying to silence cost 4.6× more (WFS prefilter IIR state).
   Now at the top of `getNextAudioBlock()` and in both worker lambdas (FTZ/DAZ is per thread).
2. **Per-player multithreading wrote shared engine state**: every player called
   `advanceGlobalSmoothers()` from `Spatializer::spatialize()` on every sample, so all workers wrote the
   same arrays (per-source cost ×25 at 12 sources) and the position ramps ran (N players + 1)× too fast.
3. **Fix**: `SpatializationEngine::advanceGlobalSmoothersForBlock()` is the single place where the global
   smoothers advance (once per block, audio thread, before any player). Players only read:
   `getBlockInterpolatedVirtualSpeakerPosition()` interpolates between block-start and block-end
   positions (`Spatializer::beginBlock()` + `m_blockFraction`). Never call `advanceGlobalSmoothers()`
   from player code again.
4. **Per-player jobs**: `m_playerJobState[]` (PENDING → RUNNING by compare-exchange → DONE),
   `tryProcessPlayerJob(i)`. The audio thread renders players too instead of spinning; no deadline, no
   player rendered twice, lambdas capture `[this, i]` only. Replaces the counter-based timeout path
   (which could double-render a player and captured a stack object by reference). Sequential fallback
   above `MAX_THREADED_PLAYERS = 256`.
5. **Crash when adding/removing a player while audio runs** (seen at 256 virtual speakers, also present
   in the old DLL): `setFilePath()` swapped the reader/resampler while the audio thread used it;
   `juce::ResamplingAudioSource::getNextAudioBlock()` on a non-prepared or released resampler divides by
   zero. Fixed with `SpatPlayer::m_sourceLock` (main thread: blocking lock around the swap in
   `setFilePath` / `prepareToPlay` / `releaseResources`; audio/worker: try-lock, player skipped for that
   block) plus `m_blockValid` and the `m_isPrepared` guard.

What the measurements say about scaling:
- Cost ∝ (source, speaker) pairs: ≈3 000 pairs on this machine (100 players @ 32 ch, 48 @ 64, 20 @ 128).
- Threading is per player, so one player's own time is a floor: at 256 virtual speakers a single source
  takes ≈43 % of a 512-sample block, overruns from 3 sources.
- Not done (deliberately set aside): computing mask + WFS delays/gains once per block instead of per
  sample (bench switch `blockwfs`: ≈ −50 % per source, but changes the rendering), splitting one
  player's channels across threads, moving file reading out of the callback
  (`AudioTransportSource` has no read-ahead thread: `setSource(…, 0, nullptr, …)`).
- HRTF downmix threading is worth keeping (1 440 µs → 400 µs per block at 32 speakers). Known defects
  left as is: with 15 chunks for 32 speakers the audio thread's own chunk is empty, and worker output is
  dropped for the block on timeout.
- In sequential mode the per-source cost still rises with the source count (≈430 → ≈850 µs from 1 to
  10 sources); cause not identified.

Tooling added:
- CMake options `AT_SPAT_PERF_PROBE` (OFF) and `AT_SPAT_COPY_TO_UNITY` (ON); preset
  `windows-x64-release-probe` builds the instrumented DLL in `Builds/VisualStudio2022_Probe/` without
  touching the Unity plugin. `At_PerfProbe.h` exists twice and must stay identical
  (`tools/perf/engine/` and `_at_wavespace_engine/Source/`).
- Bench `tools/perf/bench/at_spat_bench.exe`: `--downmix` for a stereo device, `--list-devices`,
  `--perf nomt,blockwfs,ftz,…`. It plays sound: use `--master-db -60` for a near-silent run.
- Listening test scene `Assets/MyAssets/TestClaude/TestClaude_Orbit.unity`: N copies of
  `Audio/Speech_sample.wav` orbiting the listener at 9 m, count adjustable in Play mode (up to 200).
  `TestClaude_Static100.unity` (menu TestClaude) is the variant with 100 pre-created, disabled players.
  Everything Claude adds on the Unity side goes in `Assets/MyAssets/TestClaude/`.
- A crash in the Unity Editor can be resolved to function names without a PDB: rebuild the same
  Release configuration with `-DCMAKE_SHARED_LINKER_FLAGS=/MAP`, check that `.text` has the same size,
  then look up `address − module base` (both in `Editor.log`) in the `.map`.
- Other findings: `At_MasterOutput.Awake()` does not read `binauralRenderMode` from the state file (the
  value serialized on the component is sent); the asynchronous device scan returned ASIO drivers only
  in the bench until a synchronous `AT_WS_refreshDevices(1, 0)`.
- `Unity_WaveSpace_oldVersion/` (inside the project root, ignored by Unity) holds an older engine and
  project used for comparison; it was no faster.
