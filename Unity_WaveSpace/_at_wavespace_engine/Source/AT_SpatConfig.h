/**
 * @file AT_SpatConfig.h
 * @brief Build options for AT WaveSpace
 * @author Antoine Gonot
 * @date 2026
 */

#pragma once

// ============================================================================
// CONSOLE APPLICATION MODE
// Defined externally — do not set here.
//   CMake   : cmake --preset macos-console-release  (BUILD_CONSOLE_APP=ON)
//   Projucer: add AT_SPAT_CONSOLE_APP to extraDefs in the console app exporter
// Disables async device scanning, which requires a running JUCE message loop.
// ============================================================================
// AT_SPAT_CONSOLE_APP

// ============================================================================
// UNITY DEBUG LOGGING
// Defined externally — do not set here.
//   CMake   : remove DISABLE_UNITY_LOGGING from target_compile_definitions
//   Projucer: remove DISABLE_UNITY_LOGGING from the exporter extraDefs
// WARNING: logging from the audio thread into managed C# code causes significant
// latency spikes and audio underruns. Never enable in production.
// ============================================================================
// DISABLE_UNITY_LOGGING

// ============================================================================
// MULTITHREADED PLAYER PROCESSING
// ============================================================================
// Uncomment to enable parallel WFS player processing via a thread pool.
// Requires >= 3 active players (MIN_PLAYERS_FOR_THREADING) to activate.
// WARNING: may cause audio underruns on some systems (spin-wait on audio thread).
// Leave disabled unless you have profiled a clear CPU benefit.

#define AT_SPAT_ENABLE_MULTITHREADING
