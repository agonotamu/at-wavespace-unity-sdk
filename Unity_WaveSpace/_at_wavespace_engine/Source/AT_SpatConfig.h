/**
 * @file AT_SparConfig.h
 * @brief build options to edit by the user
 * @author Antoine Gonot
 * @date 2026
 */

#pragma once

// ============================================================================
// MULTITHREADED PLAYER PROCESSING
// ============================================================================
// Uncomment to enable parallel WFS player processing via thread pool.
// Requires >= 3 players (MIN_PLAYERS_FOR_THREADING) to activate.
// WARNING: may cause audio underruns on some systems (spin-wait on audio thread).
// Leave disabled unless you have profiled a clear CPU benefit.

#define AT_SPAT_ENABLE_MULTITHREADING

// ============================================================================
// CONSOLE APP SOURCE CODE INCLUDE VS. DYNAMIC LIBRARY
// ============================================================================
// Define to use AT SPAT in a console application (no JUCE message loop).
// Disables async device scanning which requires a running message loop.

//#define AT_SPAT_CONSOLE_APP

// ============================================================================
// UNITY LOGS (CRITICAL)
// ============================================================================
// Uncomment to completely remove all log calls from the binary.
// Required for production to avoid Unity Logger overhead in the audio thread.
#define DISABLE_UNITY_LOGGING


