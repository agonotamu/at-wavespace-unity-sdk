/**
 * @file AT_SpatConfig.h
 * @brief Build options for AT WaveSpace
 * @author Antoine Gonot
 * @date 2026
 */

#pragma once

// ============================================================================
// UNITY DEBUG LOGGING
// ============================================================================
// Comment out to enable native → Unity Console logging.
// Can also be controlled via Projucer extraDefs or CMake target_compile_definitions
// without editing this file.
// WARNING: logging from the audio thread into managed C# code causes significant
// latency spikes and audio underruns. Never enable in production.

#define DISABLE_UNITY_LOGGING

// ============================================================================
// MULTITHREADED PLAYER PROCESSING
// ============================================================================
// Compiles in the thread-pool code paths: HRTF convolution threading (active) and
// parallel WFS player processing (compiled in but OFF by default since Oct 2026 —
// see SpatializationEngine constructor; setMultithreadingEnabled(true) re-enables it,
// from MIN_PLAYERS_FOR_THREADING = 3 active players).
// WARNING: may cause audio underruns on some systems (spin-wait on audio thread).
// Leave disabled unless you have profiled a clear CPU benefit.

#define AT_SPAT_ENABLE_MULTITHREADING
