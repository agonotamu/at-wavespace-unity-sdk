<p align="left">
  <img src="docs/images/WaveSpace_Logo.png" alt="AT WaveSpace Logo" width="360"/>
</p>

# Wave Field Synthesis Audio Engine

> High-precision spatial audio for Unity, powered by Wave Field Synthesis.

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20macOS-blue)](#)
[![Unity](https://img.shields.io/badge/Unity-2021%2B-black?logo=unity)](#)
[![JUCE](https://img.shields.io/badge/Built%20with-JUCE-orange)](#)

---

## Overview

**AT WaveSpace** is a native spatial audio engine based on **Wave Field Synthesis (WFS)**, developed by Antoine Gonot at the [CNRS LMA](https://www.lma.cnrs-mrs.fr/) laboratory in Marseille, France.

It enables physically grounded 3D sound field reproduction inside Unity using a high-performance **C++ / JUCE DSP core**, exposed to Unity through a pure **C API** (`extern "C"`) and a **C# wrapper layer**.

The engine targets multi-speaker WFS arrays (line / circle / square or custom configurations) and supports **Binaural Virtualization** for headphone rendering.

<p align="center">
  <img src="docs/gifs/demo.gif" alt="AT WaveSpace Demo" width="720"/>
</p>

---

## Features

| Feature | Description |
|---|---|
| **Wave Field Synthesis** | Physically-based spatial rendering over loudspeaker arrays using the standard 2.5D driving function, with pre-filter, per-speaker delay and gain |
| **Dynamic Source Positioning and Spatial Continuity** | Continuous 3D source position updates from Unity. Automatic and smooth modification of the 2.5D driving function for sources either outside/behind or inside/in front of the virtual loudspeaker array. Time-reversal for focused sources is applied with a blending function, and secondary source regularisation is applied to avoid singularities at the boundary |
| **2D vs. 3D Sources** | Support for 2D sources, i.e. multichannel audio files with an arbitrary number of channels routed directly to the virtual speaker outputs |
| **Compute Shader for Wavefront Visualisation** | A Unity prefab with a dedicated compute shader draws the wavefront of a pure tone on a plane, using the gains and delays computed for a given 3D source |
| **Binaural Virtualization** | Headphone monitoring of the loudspeaker array via per-speaker HRTF convolution |
| **Simple Binaural Mode** | HRTF-based headphone rendering, switchable at runtime |
| **Near-Field Compensation** | Both Binaural Virtualization and Simple Binaural modes support near-field compensation following the Distance Variation Function (DVF) approach |
| **Real-time DSP** | High-performance C++ / JUCE audio engine running on a dedicated thread. Source playback/spatialisation and speaker convolutions are distributed across threads according to available resources |
| **Multi-channel Output** | Supports up to 1024 virtual speakers |
| **Native Unity Integration** | Plug-and-play C# API via `At_Player` and `At_MasterOutput` components |
| **Custom Speaker Configurations** | Save and load any loudspeaker array geometry from a `.json` file |
| **Cross-platform** | Windows (ASIO / WASAPI) and macOS (CoreAudio) |

---

## Architecture

<p align="center">
  <img src="docs/images/WaveSpaceArchiLib.png" alt="AT WaveSpace Architecture" width="720"/>
</p>

The C++ core compiles to a **dynamic library** (`.dll` / `.dylib`) that Unity loads via P/Invoke. All DSP processing, speaker rendering, and device management occur in the native layer. Unity provides scene geometry and playback control.

---

## Repository Structure

```
at-wavespace-unity-sdk/
├── Unity_WaveSpace/                  # Full Unity demo project (Unity 2021+)
│   ├── Assets/
│   │   ├── AT_WaveSpace/             # C# scripts, prefabs, configuration
│   │   └── Plugins/                  # Compiled native libraries (macOS / Windows)
│   └── _at_wavespace_engine/         # JUCE C++ source — compile the lib directly here
│       ├── _at_wavespace_engine.jucer     # Projucer project — native library
│       ├── _at_wavespace_consoleApp.jucer # Projucer project — standalone console app
│       ├── CMakeLists.txt            # CMake file for Xcode or Visual Studio project generation
│       └── CMakePresets.json         # Build presets for CMake
├── UnityPackage/
│   └── At_WaveSpace_1.0.unitypackage # Ready-to-import Unity package
├── docs/
│   ├── images/
│   └── gifs/
└── LICENSE
```

---

## Installation — Unity Package (recommended)

The fastest way to get started is to import the prebuilt `.unitypackage` into your existing Unity project.

### Prerequisites

- Unity **2021.3 LTS** or later
- A multi-channel audio interface with an ASIO (Windows) or CoreAudio (macOS) driver
- A supported speaker array, or headphones for binaural mode

### Steps

1. **Download** `At_WaveSpace_1.0.unitypackage` from the [`UnityPackage/`](UnityPackage/) folder.

2. In Unity, go to **Assets → Import Package → Custom Package…** and select the downloaded file.

3. Import all assets (scripts, prefabs, native plugins, and configuration files).

4. Open the demo scene `WaveSpace_Starter.unity` located in `Assets/At_WaveSpace/Scenes/`.

5. Press **Play**. The native DSP core will initialise automatically.

---

## Settings via Unity Editor

### At_MasterOutput Settings

<table><tr>
<td width="40%" valign="middle"><img src="docs/images/MasterOutputEditor.png" width="100%"/></td>
<td valign="top"><small>
<dl>
<dt><b>Audio Device</b></dt><dd>Output device dropdown. Lists all devices with at least one output channel (ASIO/WASAPI on Windows, CoreAudio on macOS). Displays the maximum output channel count of the selected device below the dropdown.</dd>
<dt><b>Master Gain (dB)</b></dt><dd>Global output gain, −80 to +10 dB. Slider and text field.</dd>
<dt><b>Makeup Gain (dB)</b></dt><dd>Additional post-processing gain applied after the WFS mix, −10 to +40 dB. Useful for compensating level differences between modes.</dd>
<dt><b>Binaural Virtualization</b></dt><dd>When enabled, the full WFS multichannel output is convolved speaker-by-speaker with HRTFs and downmixed to stereo (2 channels). Automatically enabled if the number of virtual speakers exceeds the device's physical output channel count.</dd>
<dt><b>HRTF File</b></dt><dd>Path to the HRTF impulse response file (<code>.txt</code>), stored relative to <code>StreamingAssets/</code>. A "Load HRTF File" button opens a file browser. Visible only when Binaural Virtualization is enabled.</dd>
<dt><b>Simple Binaural Mode (A/B Test)</b></dt><dd>Bypasses WFS synthesis and applies HRTF directly to each source for A/B listening comparisons. Only available when Binaural Virtualization is enabled.</dd>
<dt><b>Near-Field Correction (NFC)</b></dt><dd>Applies a rigid-sphere ILD correction (DVF, Duda &amp; Martens 1998) to the binaural output. Only available when Binaural Virtualization is enabled.</dd>
<dt><b>HRTF Distance (m)</b></dt><dd>Measurement distance of the loaded BRIR set (typically 1.0 m). Used by NFC to compute the distance-dependent correction.</dd>
<dt><b>Buffer Size</b></dt><dd>Audio buffer size: 64, 128, 256, 512, 1024, or 2048 samples. Lower values reduce latency but increase CPU load.</dd>
<dt><b>Sampling Rate</b></dt><dd>44100 or 48000 Hz.</dd>
<dt><b>Speaker Configuration</b></dt><dd>Array topology: <code>1D</code>, <code>2D SQUARE</code>, <code>2D HALF-SQUARE</code>, <code>2D CIRCLE</code>, <code>2D HALF-CIRCLE</code>, or <code>Custom</code> (loaded from a <code>.spatconfig</code> file). After selecting a standard topology, the number of virtual speakers and rig size can be set below.</dd>
<dt><b>Num. Virtual Speakers</b></dt><dd>Number of virtual speakers for standard configurations (max 1024). If this exceeds the device's physical output channel count and Binaural Virtualization is off, the engine automatically switches to Binaural Virtualization.</dd>
<dt><b>Speaker Rig Size (m)</b></dt><dd>Total span of the speaker array (distance from first to last speaker), 0.1–80 m. For <code>Custom</code> configs, this is read from the <code>.spatconfig</code> file.</dd>
<dt><b>Max Distance / Delay (m)</b></dt><dd>Maximum source-to-speaker distance (10–100 m) used to scale per-speaker delays. Set this to the largest expected propagation distance in your setup.</dd>
</dl>
</small></td>
</tr></table>

---

### At_Player Settings — 3D Source

<table><tr>
<td width="40%" valign="middle"><img src="docs/images/AtPlayer3DEditor.png" width="100%"/></td>
<td valign="top"><small>
<dl>
<dt><b>Audio File</b></dt><dd>"Open" button browses to <code>StreamingAssets/Audio/</code> and accepts <code>.wav</code>, <code>.mp3</code>, or <code>.ogg</code> files. The channel count is read from the file's metadata automatically. The path is stored relative to <code>StreamingAssets/</code> for cross-platform portability.</dd>
<dt><b>Gain (dB)</b></dt><dd>Per-source gain, −80 to +10 dB. Slider and text field.</dd>
<dt><b>Spatialization Mode</b></dt><dd>Toggle between <b>3D (WFS)</b> — full wave field synthesis spatialisation — and <b>2D (Direct)</b> — multichannel direct routing to virtual speakers.</dd>
<dt><b>Play On Awake</b></dt><dd>Start playback automatically when the scene starts.</dd>
<dt><b>Loop</b></dt><dd>Loop the audio file continuously.</dd>
<dt><b>Playback Speed</b></dt><dd>Playback rate multiplier, 0.1× to 4.0×.</dd>
<dt><b>Minimum Distance (m)</b></dt><dd>Distance below which amplitude attenuation is clamped (0.1–50 m). Prevents the gain from diverging as the source approaches a virtual speaker.</dd>
<dt><b>Attenuation</b></dt><dd>Exponent α of the <code>1/d^α</code> distance law (0–2). Set to <code>0</code> to disable distance attenuation entirely.</dd>
</dl>
</small></td>
</tr></table>

---

### At_Player Settings — 2D Source

<table><tr>
<td width="40%" valign="middle"><img src="docs/images/AtPlayer2DEditor.png" width="100%"/></td>
<td valign="top"><small>
<dl>
<dt><b>Audio File</b></dt><dd>"Open" button browses to <code>StreamingAssets/Audio/</code>. The file's channel count is read from metadata and used to determine how many virtual speaker outputs are fed.</dd>
<dt><b>Gain (dB)</b></dt><dd>Per-source gain, −80 to +10 dB. Slider and text field.</dd>
<dt><b>Spatialization Mode</b></dt><dd>Set to <b>2D (Direct)</b>: each channel of the audio file is routed directly to the corresponding virtual speaker output, bypassing WFS spatialisation. Useful for pre-rendered content or direct speaker feeds.</dd>
<dt><b>Play On Awake</b></dt><dd>Start playback automatically when the scene starts.</dd>
<dt><b>Loop</b></dt><dd>Loop the audio file continuously.</dd>
<dt><b>Playback Speed</b></dt><dd>Playback rate multiplier, 0.1× to 4.0×.</dd>
</dl>
</small></td>
</tr></table>

---

### Advanced Settings

<table><tr>
<td width="40%" valign="middle"><img src="docs/images/AdvancedSettings.png" width="100%"/></td>
<td valign="top"><small>
<p>Accessible via <b>AT_WaveSpace → Advanced Settings…</b>. Groups engine-wide and per-player parameters that apply globally to the scene.</p>
<p><b>MASTER</b></p>
<dl>
<dt><b>Simple Binaural Mode (A/B Test)</b></dt><dd>Bypasses WFS synthesis and applies HRTF directly to each source. Used for A/B perceptual comparisons. Only available when Binaural Virtualization is enabled in <code>At_MasterOutput</code>.</dd>
<dt><b>Near-Field Correction (NFC)</b></dt><dd>Applies a rigid-sphere ILD correction (DVF, Duda &amp; Martens 1998) to the binaural output. Only available when Binaural Virtualization is enabled.</dd>
<dt><b>HRTF Distance (m)</b></dt><dd>Measurement distance of the loaded BRIR set (typically 1.0 m). Source distance and azimuth are derived automatically from scene transforms.</dd>
<dt><b>Add / Remove Wavefront Display</b></dt><dd>Instantiates or removes the <code>WavefrontDisplay</code> prefab in the scene. The display plane is automatically scaled to match the Speaker Rig Size.</dd>
</dl>
<p><b>PLAYERS</b> <em>(applied to all players)</em></p>
<dl>
<dt><b>Enable WFS Output Mask</b></dt><dd>Activates only the speakers on the correct side of the perpendicular to the listener-source line passing through the virtual source.</dd>
<dt><b>Pre-filtering √(jω)</b></dt><dd>Applies a half-derivative IIR prefilter to correct the +3 dB/oct amplitude error inherent in 2.5D WFS synthesis.</dd>
<dt><b>WFS Gain cos(φ)/√r</b></dt><dd>Applies per-speaker 2.5D amplitude weighting. Disable for delay-only WFS rendering.</dd>
<dt><b>Active-speakers min/max (focused sources)</b></dt><dd>Restricts the focused-source time-reversal delay reference to active speakers only. Prevents left/right inversion when the WFS Output Mask is active.</dd>
<dt><b>Source Size ε (m)</b></dt><dd>Secondary source regularisation radius shared by the audio engine and the wavefront shader. P1: replaces r in the amplitude term with r_eff = √(r²+ε²) to avoid divergence near the array plane. P2: replaces the hard speaker-activation gate with a C¹ raised-cosine taper. Set to 0 for ideal point source behaviour.</dd>
</dl>
</small></td>
</tr></table>

---

### Mixer

<table><tr>
<td width="40%" valign="top"><img src="docs/images/Mixer.png" width="100%"/></td>
<td valign="top"><small>
<p>The Mixer window provides per-source and master gain control, as well as individual <b>Play / Stop</b> buttons for each active source — useful for testing and live scene editing without leaving the Unity Editor.</p>
<p>Each source strip displays its name, current gain (in dB), and playback state. The master strip at the bottom controls the global output level.</p>
<p><em>Note: in the screenshot above, the master bus shows only <b>two output channels</b> because the engine is running in <b>Binaural Virtualization</b> mode, which collapses the WFS multichannel output to a stereo headphone mix. In WFS mode, all active speaker channels are shown instead.</em></p>
</small></td>
</tr></table>

---

### Wavefront Display Settings

<table>
<tr>
<td width="45%">
<img src="docs/images/WavefrontDisplayEditor.png"/>
</td>
<td valign="top" width="55%">

| Parameter | Description |
|---|---|
| **RenderTexture Resolution** | Resolution (in pixels) of the render texture on which the wavefront is drawn. Higher values produce a sharper result but increase GPU memory usage. |
| **Wave Frequency** | Frequency (Hz) of the simulated pure tone whose wavefront is displayed. Lower frequencies show large, slow wavefronts; higher frequencies reveal spatial aliasing artefacts. |

</td>
</tr>
</table>

The three renders below show the same WFS source at 250 Hz, 1 kHz, and 2 kHz. The 2 kHz image illustrates the onset of spatial aliasing above the array's aliasing frequency (~920 Hz for a 32-speaker array at 18.5 cm spacing).

<table>
<tr>
<td align="center" width="33%">
<img src="docs/images/Wavefront_250.png" width="100%"/><br/>
<sub><b>250 Hz</b> — Long wavelength, smooth wavefront, no aliasing</sub>
</td>
<td align="center" width="33%">
<img src="docs/images/Wavefront_1000.png" width="100%"/><br/>
<sub><b>1000 Hz</b> — Near the aliasing limit, wavefront begins to distort</sub>
</td>
<td align="center" width="33%">
<img src="docs/images/Wavefront_2000.png" width="100%"/><br/>
<sub><b>2000 Hz</b> — Above aliasing frequency, grating lobes clearly visible</sub>
</td>
</tr>
</table>

---

## Recompiling the Native Library from Source

The JUCE C++ source is located under `Unity_WaveSpace/_at_wavespace_engine/`. Two build workflows are available: **Projucer** (traditional) and **CMake** (recommended for command-line and CI use). Both output the compiled library directly to the Unity `Plugins/` folder.

### Prerequisites

- **Windows:** Visual Studio 2019 or 2022 with the C++ Desktop workload; ASIO SDK
- **macOS:** Xcode 13+ with Command Line Tools
- **Projucer workflow:** [JUCE 7+](https://juce.com/) with Projucer
- **CMake workflow:** CMake 3.22+; no Projucer required

---

### Option A — Projucer

#### 1. Open the Projucer project

Open `_at_wavespace_engine/_at_wavespace_engine.jucer` in Projucer.

#### 2. Set the JUCE path

In Projucer, go to **File → Global Paths** and set **Path to JUCE** to your local JUCE installation.

#### 3. Configure the exporter

Select the exporter for your platform (Visual Studio or Xcode). Verify that the **Binary Location** points to:

```
../../Assets/Plugins/
```

This ensures the compiled `.dll` / `.dylib` is written directly into the Unity project's plugin folder.

#### 4. Save and open in IDE

Click **Save Project and Open in IDE** in Projucer.

#### 5. Build

- **Windows:** Build in **Release x64** configuration.
- **macOS:** Build the **Release** scheme. For a Universal Binary (Apple Silicon + Intel), set `ARCHS = arm64 x86_64` in Build Settings.

#### 6. Refresh Unity

Back in Unity, click anywhere in the Project window to trigger an asset refresh. Unity will automatically re-import the updated native library.

---

### Option B — CMake

The repository includes a `CMakeLists.txt` and a `CMakePresets.json` under `_at_wavespace_engine/`, providing a self-contained build without Projucer.

#### 1. Navigate to the source directory

```bash
cd Unity_WaveSpace/_at_wavespace_engine
```

#### 2. List available presets

```bash
cmake --list-presets
```

#### 3. Configure and build

Select the preset that matches your platform and target. For example, to build a macOS Universal Binary (arm64 + x86_64):

```bash
cmake --preset mac-release-universal
cmake --build --preset mac-release-universal
```

For a Windows Release x64 build:

```bash
cmake --preset windows-release-x64
cmake --build --preset windows-release-x64
```

> **macOS note:** The presets configure `CMAKE_OSX_ARCHITECTURES="x86_64;arm64"` and set the deployment target to macOS 11.0 automatically.

#### 4. Refresh Unity

The compiled library is written to `Assets/Plugins/`. Switch back to Unity and click anywhere in the Project window to trigger an asset refresh.

---

## Console Application

A standalone **console test application** is also included for validating the DSP core without Unity.

Open `_at_wavespace_engine/_at_wavespace_consoleApp.jucer` in Projucer (or use the corresponding CMake target) and follow the same build steps as above. The console app initialises the `AudioDeviceManager` directly and is useful for offline corpus generation, calibration, and audio routing diagnostics.

> **Note (macOS / Windows):** Console apps built with JUCE require a `ScopedJuceInitialiser_GUI` to be instantiated first in `main()`, even when running headlessly.

---

## C# API Reference

### `At_MasterOutput`

Attach to one GameObject per scene. Manages device initialisation, channel routing, and the WFS/Binaural mode.

| Property | Type | Description |
|---|---|---|
| `outputDeviceName` | `string` | Target audio device name |
| `speakerConfig` | `SpeakerConfig` | `Linear32`, `Linear16`, or `Custom` |
| `spatializationMode` | `SpatMode` | `WFS` or `SimpleBinaural` |
| `masterGain` | `float` | Global output gain (linear) |

### `At_Player`

Attach to any sound-emitting GameObject.

| Method | Description |
|---|---|
| `Play()` | Start playback |
| `Stop()` | Stop and rewind |
| `SetPosition(Vector3)` | Update 3D source position in real-time |
| `SetGain(float)` | Per-source gain (linear) |
| `LoadClip(AudioClip)` | Load a new audio file at runtime |

---

## Advanced Configuration

### Speaker Array Geometry

For non-standard arrays, set `speakerConfig` to `Custom` and define your geometry in `SpatialConfiguration.json`:

```json
{
  "speakerConfig": "Custom",
  "speakers": [
    { "index": 0, "x": -2.9675, "y": 0.0, "z": 0.0 },
    { "index": 1, "x": -2.7825, "y": 0.0, "z": 0.0 },
    ...
  ]
}
```

### WFS ↔ Binaural Crossfade

Mode transitions use an equal-power crossfade over a configurable number of blocks, ensuring glitch-free switching at runtime. Set the fade duration via `At_MasterOutput.fadeDurationMs`.

### Near-Field Compensation (NFC)

The WFS renderer includes a Near-Field Compensation filter. The reference distance `rRef` is set in `AT_SpatializationEngine` (default: `3.0 m`). Adjust it to match your listening distance for correct level compensation.

---

## Requirements Summary

| | Windows | macOS |
|---|---|---|
| Compiler | Visual Studio 2019/2022 | Xcode 13+ |
| Audio API | ASIO, WASAPI | CoreAudio |
| Unity | 2021.3 LTS+ | 2021.3 LTS+ |
| Architecture | x64 | x64, Apple Silicon |
| JUCE | 7+ | 7+ |
| CMake (optional) | 3.22+ | 3.22+ |

---

## License

This project is released under the [MIT License](LICENSE).

The DSP algorithms are based on WFS research conducted at **CNRS LMA** (Laboratoire de Mécanique et d'Acoustique), Marseille.

---

## Acknowledgements

- [CNRS LMA](https://www.lma.cnrs-mrs.fr/) — Research foundation for WFS rendering
- [JUCE](https://juce.com/) — Cross-platform C++ audio framework
- KEMAR mannequin measurements — Head-Related Transfer Function (HRTF) dataset

---

*Built by [Antoine Gonot](https://github.com/agonotamu)*
