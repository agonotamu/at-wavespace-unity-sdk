/**
 * @file UnityLogger.h
 * @brief Logging system forwarding messages to the Unity Console with compile-time disable support
 *
 * @par Usage
 * @code
 * LOG("Spat Engine created!");
 * LOG("Number of channels: " << m_numOutputChannels);
 * LOG_WARNING("Buffer size is suboptimal");
 * LOG_ERROR("Failed to load audio file");
 * @endcode
 *
 * @par Performance note
 * Logging in the audio thread causes significant performance degradation (1–5 ms per call).
 * Define DISABLE_UNITY_LOGGING to completely strip all log calls from the binary.
 * This is essential for production builds.
 *
 * @author Antoine Gonot
 * @date 2025
 */

#pragma once
#include <string>
#include <sstream>
#include <mutex>
#include "AT_SpatConfig.h"


/// Callback type used by Unity to receive log messages from the native plugin.
typedef void (*UnityLogCallback)(const char* message);

class UnityLogger
{
public:
    static UnityLogger& getInstance();

    void setCallback(UnityLogCallback callback);

    void log(const std::string& message);
    void logWarning(const std::string& message);
    void logError(const std::string& message);

private:
    UnityLogger() = default;
    ~UnityLogger() = default;

    UnityLogCallback m_callback = nullptr;
    std::mutex m_mutex;
};

// ============================================================================
// MACROS WITH COMPILE-TIME DISABLE
// ============================================================================

#ifdef DISABLE_UNITY_LOGGING
    // PRODUCTION: log calls compile to no-ops (zero overhead)
    #define LOG(message)         ((void)0)
    #define LOG_WARNING(message) ((void)0)
    #define LOG_ERROR(message)   ((void)0)
#else
    // DEBUG: log calls are active — use only during development
    #define LOG(message) \
        do { \
            std::ostringstream oss; \
            oss << message; \
            UnityLogger::getInstance().log(oss.str()); \
        } while(0)

    #define LOG_WARNING(message) \
        do { \
            std::ostringstream oss; \
            oss << message; \
            UnityLogger::getInstance().logWarning(oss.str()); \
        } while(0)

    #define LOG_ERROR(message) \
        do { \
            std::ostringstream oss; \
            oss << message; \
            UnityLogger::getInstance().logError(oss.str()); \
        } while(0)
#endif

// ============================================================================
// PERFORMANCE IMPACT REFERENCE
// ============================================================================
/*
Without DISABLE_UNITY_LOGGING (debug):
  - Unity Editor: 95% CPU, 2 channels with glitches (logging kills performance)
  - Unity Build:  63% CPU, 20 channels with glitches

With DISABLE_UNITY_LOGGING (production):
  - Unity Editor: 15% CPU, 110 channels OK
  - Unity Build:  12% CPU, 110 channels OK

Never log inside the audio thread (getNextAudioBlock, processAndAccumulate).
*/
