/**
 * @file PluginInterface.h
 * @brief C interface for the audio spatialization library
 * @author Antoine Gonot
 * @date 2026
 */

#pragma once

 // Automatically detect Windows platform
#if defined(_WIN32) || defined(_WIN64)
    #define EXPORT_API __declspec(dllexport)
    #define CALL_CONV __stdcall
#elif defined(__APPLE__)
    #define EXPORT_API __attribute__((visibility("default")))
    #define CALL_CONV
#else
    #define EXPORT_API
    #define CALL_CONV
#endif

// ============================================================================
// ERROR CODES
// ============================================================================

/**
 * @brief Success code
 */
#define AUDIO_PLUGIN_OK 0

/**
 * @brief Error code
 */
#define AUDIO_PLUGIN_ERROR 1

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// LOGGING
// ============================================================================

/**
 * @brief Sets the logging callback for Unity
 * @param callback Callback function that will be called for each log message
 * 
 * This function allows Unity to receive log messages from the native plugin.
 * The callback will be called with C strings (const char*) for each message.
 * 
 * Example usage in C#:
 * @code
 * [DllImport("AT_WS_AudioEngineLib")]
 * private static extern void AT_WS_setLogCallback(Action<string> callback);
 * 
 * void Start() {
 *     AT_WS_setLogCallback((message) => Debug.Log(message));
 * }
 * @endcode
 */
EXPORT_API void CALL_CONV AT_WS_setLogCallback(void (*callback)(const char*));

// ============================================================================
// PLUGIN MANAGEMENT
// ============================================================================

/**
 * @brief Initializes the audio plugin
 * 
 * Creates the global AudioManager instance and initializes JUCE.
 * This function MUST be called before any other plugin functions.
 * Thread-safe with internal mutex.
 * 
 * @return AUDIO_PLUGIN_OK if successful, AUDIO_PLUGIN_ERROR otherwise
 */
EXPORT_API int CALL_CONV AT_WS_initialize();

/**
 * @brief Configures the audio device and starts the engine
 * 
 * @param audioDeviceName Device name (empty = default)
 * @param inputChannels Number of input channels
 * @param outputChannels Number of output channels
 * @param bufferSize Buffer size (0 = auto)
 * @param isBinauralVirtualization value of the flag for binaural virtualization
 * @return AUDIO_PLUGIN_OK if successful, AUDIO_PLUGIN_ERROR otherwise
 */
EXPORT_API int CALL_CONV AT_WS_setup(const char* audioDeviceName, int inputChannels, int outputChannels, int bufferSize, bool isBinauralVirtualization);

/**
 * @brief Stops the audio engine
 * 
 * @return AUDIO_PLUGIN_OK if successful, AUDIO_PLUGIN_ERROR otherwise
 */
EXPORT_API int CALL_CONV AT_WS_stop();

/**
 * @brief Completely closes the plugin
 * 
 * Destroys the AudioManager instance and stops JUCE.
 * Thread-safe with internal mutex.
 * 
 * @return AUDIO_PLUGIN_OK if successful, AUDIO_PLUGIN_ERROR otherwise
 */
EXPORT_API int CALL_CONV AT_WS_shutdown();

// ============================================================================
// AUDIO DEVICE ENUMERATION
// ============================================================================

/**
 * @brief Returns the number of available audio devices
 * 
 * @return Number of devices
 */
EXPORT_API int CALL_CONV AT_WS_getDeviceCount();

/**
 * @brief Returns the name of an audio device by its index
 * 
 * @param deviceIndex Device index
 * @param deviceName Buffer to store the name (max 256 characters)
 * @return AUDIO_PLUGIN_OK if successful, AUDIO_PLUGIN_ERROR otherwise
 */
EXPORT_API int CALL_CONV AT_WS_getDeviceName(int deviceIndex, char* deviceName);

/**
 * @brief Returns the number of channels for a device
 * 
 * @param deviceIndex Device index
 * @param isOutput 0 = inputs, non-zero = outputs
 * @return Number of channels, or -1 on error
 */
EXPORT_API int CALL_CONV AT_WS_getDeviceChannels(int deviceIndex, int isOutput);

// ============================================================================
// PLAYER MANAGEMENT
// ============================================================================

/**
 * @brief Adds a new player to the system
 * 
 * @param uid Pointer to receive the unique player ID
 * @param is3D true = 3D, false = 2D
 * @param isLooping true = loop playback
 * @return AUDIO_PLUGIN_OK if successful, AUDIO_PLUGIN_ERROR otherwise
 */
EXPORT_API int CALL_CONV AT_WS_addPlayer(int* uid, bool is3D, bool isLooping);

/**
 * @brief Removes a player
 * 
 * @param uid Player ID
 * @return AUDIO_PLUGIN_OK if successful, AUDIO_PLUGIN_ERROR otherwise
 */
EXPORT_API int CALL_CONV AT_WS_removePlayer(int uid);

/**
 * @brief Loads an audio file into a player
 * 
 * @param uid Player ID
 * @param filepath Path to the audio file
 * @return AUDIO_PLUGIN_OK if successful, AUDIO_PLUGIN_ERROR otherwise
 */
EXPORT_API int CALL_CONV AT_WS_setPlayerFilePath(int uid, const char* filepath);

/**
 * @brief Retrieves metadata from an audio file (NEW FUNCTION)
 * 
 * This function only reads the file headers to extract:
 * - Number of channels
 * - Sample rate
 * - Duration in seconds
 * - Total number of samples
 * 
 * It is much faster than loading the entire file.
 * 
 * IMPORTANT: AT_WS_initialize() MUST be called before this function
 * for JUCE to be initialized. If the manager is not initialized,
 * this function will return AUDIO_PLUGIN_ERROR.
 * 
 * @param filepath Path to the audio file
 * @param numChannels Pointer to receive the number of channels
 * @param sampleRate Pointer to receive the sample rate
 * @param lengthSeconds Pointer to receive the duration in seconds
 * @param totalSamples Pointer to receive the total number of samples
 * @return AUDIO_PLUGIN_OK if successful, AUDIO_PLUGIN_ERROR otherwise
 * 
 * Possible error codes:
 * - Manager not initialized (call AT_WS_initialize() first)
 * - File not found
 * - Unsupported format (supported: WAV, AIFF, FLAC, OGG, MP3)
 * - Corrupted file
 */
EXPORT_API int CALL_CONV AT_WS_getAudioFileMetadata(
    const char* filepath,
    int* numChannels,
    double* sampleRate,
    double* lengthSeconds,
    long long* totalSamples
);

// ============================================================================
// PLAYBACK CONTROL
// ============================================================================

/**
 * @brief Starts playback for a player
 * 
 * @param uid Player ID
 * @return AUDIO_PLUGIN_OK if successful, AUDIO_PLUGIN_ERROR otherwise
 */
EXPORT_API int CALL_CONV AT_WS_startPlayer(int uid);

/**
 * @brief Stops playback for a player
 * 
 * @param uid Player ID
 * @return AUDIO_PLUGIN_OK if successful, AUDIO_PLUGIN_ERROR otherwise
 */
EXPORT_API int CALL_CONV AT_WS_stopPlayer(int uid);

/**
 * @brief Starts all players
 * 
 * @return AUDIO_PLUGIN_OK if successful, AUDIO_PLUGIN_ERROR otherwise
 */
EXPORT_API int CALL_CONV AT_WS_startAllPlayers();

/**
 * @brief Stops all players
 * 
 * @return AUDIO_PLUGIN_OK if successful, AUDIO_PLUGIN_ERROR otherwise
 */
EXPORT_API int CALL_CONV AT_WS_stopAllPlayers();

/**
 * @brief Sets the pre-filtering state
 *
 * @param isPrefilter True if pre-filtering is applied
 * @return AUDIO_PLUGIN_OK if successful, AUDIO_PLUGIN_ERROR otherwise
 */
EXPORT_API int CALL_CONV AT_WS_setIsPrefilterAllPlayers(bool isPrefilter);

// ============================================================================
// PLAYER PARAMETERS
// ============================================================================

/**
 * @brief Sets the position and rotation of a player
 * 
 * @param uid Player ID
 * @param position Array [x, y, z]
 * @param rotation Array [x, y, z] (Euler angles)
 * @param forward Array [x, y, z] (forward vector)
 * @return AUDIO_PLUGIN_OK if successful, AUDIO_PLUGIN_ERROR otherwise
 */
EXPORT_API int CALL_CONV AT_WS_setPlayerTransform(int uid, float* position, float* rotation, float* forward);

/**
 * @brief Sets the audio parameters of a player
 * 
 * @param uid Player ID
 * @param gain Linear gain
 * @param playbackSpeed Playback speed
 * @param attenuation Attenuation type (0 = none, >0 = 1/pow(distance, attenuation))
 * @param minDistance Minimum distance for attenuation
 * @return AUDIO_PLUGIN_OK if successful, AUDIO_PLUGIN_ERROR otherwise
 */
EXPORT_API int CALL_CONV AT_WS_setPlayerParams(int uid, float gain, float playbackSpeed, float attenuation, float minDistance);

/**
 * @brief Sets the real-time parameters of a player (alias of setPlayerParams)
 * 
 * This function is identical to AT_WS_setPlayerParams.
 * It exists for compatibility with the old function name.
 * 
 * @param uid Player ID
 * @param gain Linear gain
 * @param playbackSpeed Playback speed
 * @param attenuation Attenuation type (0 = none, >0 = 1/pow(distance, attenuation))
 * @param minDistance Minimum distance for attenuation
 * @return AUDIO_PLUGIN_OK if successful, AUDIO_PLUGIN_ERROR otherwise
 */
EXPORT_API int CALL_CONV AT_WS_setPlayerRealTimeParameter(int uid, float gain, float playbackSpeed,
                                                            float attenuation, float minDistance);

/**
 * @brief Sets the flag enabling or disabling the speaker activation mask  applied to the output channels of the WFS algorithm
 *
 * The mask depend on the position of the player and the listener in the scene
 *
 * @param uid Player ID
 * @param isWfsSpeakerMask the flag to set for a given player
 */
EXPORT_API int CALL_CONV AT_WS_enableAllPlayersSpeakerMask(bool isWfsSpeakerMask);

/**
 * @brief Enables or disables per-speaker WFS amplitude weighting for all players.
 *
 * When enabled, the driving function applies cos(φ) / √r as an amplitude weight
 * for each virtual speaker, where φ is the angle between the speaker outward
 * normal and the source–speaker vector, and r is the source–speaker distance.
 * When disabled, all speaker gains are held at 1.0 (delay-only WFS).
 *
 * Gain transitions are smoothed (50 ms ramp) to avoid clicks on position updates.
 *
 * @param isWfsGain  true = apply WFS gain formula, false = unity gain
 * @return AUDIO_PLUGIN_OK if successful, AUDIO_PLUGIN_ERROR otherwise
 */
EXPORT_API int CALL_CONV AT_WS_setIsWfsGain(bool isWfsGain);
EXPORT_API int CALL_CONV AT_WS_setIsActiveSpeakersMinMax(bool enabled);

/**
 * @brief Sets the effective source radius for WFS singularity regularisation.
 *
 * Activates two complementary fixes simultaneously:
 *   P1 — Amplitude: r_eff = sqrt(r²+ε²) prevents cos(φ)/sqrt(r) → ∞ at the
 *        array plane. The delay path uses r_raw so phase is preserved.
 *   P2 — Mask taper: replaces the hard speaker-activation gate (dotSource < 0)
 *        with a C¹ raised-cosine ramp over ±ε, eliminating the silence step
 *        when the source crosses the array.
 *
 * The same value is forwarded to the visual shader by At_MasterOutput so the
 * wavefront display remains coherent with the audio engine.
 *
 * @param secondarySourceSize  Effective radius in metres. 0 = point source (original).
 *                    Typical: 0.1–0.5 m. Default: 0.3 m.
 * @return AUDIO_PLUGIN_OK on success, AUDIO_PLUGIN_ERROR if engine not ready.
 */
EXPORT_API int CALL_CONV AT_WS_setSecondarySourceSize(float secondarySourceSize);

// ============================================================================
// INFORMATION RETRIEVAL
// ============================================================================

/**
 * @brief Get the delay in seconds applied in the driving function for each output channel for a given player
 *
 * @return AUDIO_PLUGIN_OK (0) if successful, AUDIO_PLUGIN_ERROR (1) if failed
 *
 * @param uid Unique identifier of the player
 * @param delay Pointer to the array of delays to fill
 * @param arraySize Size of the array (this should be the number of output channels)
 */
EXPORT_API int CALL_CONV AT_WS_getPlayerWfsDelay(int uid, float* delay, int arraySize);

/**
 * @brief Get the amplitude factor applied in the driving function for each output channel for a given player
 *
 * @return AUDIO_PLUGIN_OK (0) if successful, AUDIO_PLUGIN_ERROR (1) if failed
 *
 * @param uid Unique identifier of the player
 * @param linGain Pointer to the array of gain (linear) to fill
 * @param arraySize Size of the array (this should be the number of output channels)
 */
EXPORT_API int CALL_CONV AT_WS_getPlayerWfsLinGain(int uid, float* linGain, int arraySize);

/**
 * @brief Retrieves the RMS meters of the player
 * 
 * @param uid Player ID
 * @param meters Buffer for RMS values (dB)
 * @param arraySize Buffer size
 * @return AUDIO_PLUGIN_OK if successful, AUDIO_PLUGIN_ERROR otherwise
 */
EXPORT_API int CALL_CONV AT_WS_getPlayerMeters(int uid, float* meters, int arraySize);

/**
 * @brief Retrieves the number of channels in the loaded file
 * 
 * @param uid Player ID
 * @param numChannel Pointer to receive the number of channels
 * @return AUDIO_PLUGIN_OK if successful, AUDIO_PLUGIN_ERROR otherwise
 */
EXPORT_API int CALL_CONV AT_WS_getPlayerNumChannel(int uid, int* numChannel);

/**
 * @brief Set an activation flag for each output channel of the Wave Field Synthesis spatialization algorithm for a given player
 *
 * @return AUDIO_PLUGIN_OK (0) if successful, AUDIO_PLUGIN_ERROR (1) if failed
 *
 * @param uid Unique identifier of the player
 * @param speakerMask Pointer to the array of flags (0 or 1) to fill
 * @param arraySize Size of the array (this should be the number of output channels)
 */
EXPORT_API int CALL_CONV AT_WS_getPlayerSpeakerMask(int uid, float* speakerMask, int arraySize);

/**
 * @brief Retrieves the mixer output meters
 * 
 * @param meters Buffer for RMS values (dB)
 * @param arraySize Buffer size (must match the number of output channels)
 * @return AUDIO_PLUGIN_OK if successful, AUDIO_PLUGIN_ERROR otherwise
 */
EXPORT_API int CALL_CONV AT_WS_getMixerOutputMeters(float* meters, int arraySize);

// ============================================================================
// GLOBAL PARAMETERS
// ============================================================================

/**
 * @brief Sets the listener position
 * 
 * @param position Array [x, y, z]
 * @param rotation Array [x, y, z] (Euler angles)
 * @param forward Array [x, y, z] (forward vector)
 * @return AUDIO_PLUGIN_OK if successful, AUDIO_PLUGIN_ERROR otherwise
 */
EXPORT_API int CALL_CONV AT_WS_setListenerTransform(float* position, float* rotation, float* forward);

/**
 * @brief Set the position and orientation of the virtual speaker
 *
 * This is used by the spatialization engine to setup the spatial configuration of the physical output system
 *  Delays (and eventually Gains) for the driving function for a given player is calculated according to the position
 *  Euler angles or forwards vectors can be used to know if the player or the listener is in front or behind the virtual microphones
 *
 * @return AUDIO_PLUGIN_OK (0) if successful, AUDIO_PLUGIN_ERROR (1) if failed
 *
 * @param positions Pointer to an array of 3 x N coordinates (x1, y1, z1, ..., xN, yN, zN) in left-handed coordinate system, given the position of each virtual microphone
 * @param rotations Pointer to an array of 3 x N coordinates (x1, y1, z1, ..., xN, yN, zN) in left-handed coordinate system, given the euler angles of the rotation of each virtual microphone
 * @param forwards Pointer to an array of 3 x N coordinates (x1, y1, z1, ..., xN, yN, zN) in left-handed coordinate system, given the forward vector of each virtual microphone
 * @param virtualSpeakerCount Number of virtual speakers in the scene
 */
EXPORT_API int CALL_CONV AT_WS_setVirtualSpeakerTransform(float* positions, float* rotations, float* forwards, int virtualSpeakerCount);

/**
 * @brief Set the maximum distance for a source from a virtual microphone used to setup the size of the delay line
 *
 * If the source is beyond this distance, the delay value is clipped to the largest possible value
 *
 * @return AUDIO_PLUGIN_OK (0) if successful, AUDIO_PLUGIN_ERROR (1) if failed*
 *
 * @param maxDistance Value of the maximum distance
 */
EXPORT_API int CALL_CONV AT_WS_setMaxDistanceForDelay(float maxDistance);

/**
 * @brief Sets the master gain
 * 
 * @param masterGain Linear gain
 * @return AUDIO_PLUGIN_OK if successful, AUDIO_PLUGIN_ERROR otherwise
 */
EXPORT_API int CALL_CONV AT_WS_setMasterGain(float masterGain);

/**
 * @brief Sets the makeup gain to compensate the level lost when the overall gain is normalize depanding of the number of virtual speaker and active channel when computing WFS
 *
 * @param masterGain Linear gain
 * @return AUDIO_PLUGIN_OK if successful, AUDIO_PLUGIN_ERROR otherwise
 */
EXPORT_API int CALL_CONV AT_WS_setMakeupMasterGain(float makeupMasterGain);


// ============================================================================
// OPTIMIZED DEVICE SCANNING (NEW)
// ============================================================================

/**
 * @brief Check if device scan is complete
 * @return 1 if scan is finished, 0 if still in progress
 */
EXPORT_API int CALL_CONV AT_WS_isDeviceScanComplete();

/**
 * @brief Wait for device scan to complete (blocking)
 * @param timeoutMs Maximum time to wait in milliseconds (0 = infinite)
 * @return 1 if scan completed successfully, 0 if timeout
 */
EXPORT_API int CALL_CONV AT_WS_waitForDeviceScan(int timeoutMs);

/**
 * @brief Refresh the device cache with options
 * @param includeASIO 1 to scan ASIO devices (slower), 0 to exclude
 * @param async 1 for async scan (non-blocking), 0 for sync scan (blocking)
 */
EXPORT_API void CALL_CONV AT_WS_refreshDevices(int includeASIO, int async);

/**
 * @brief Get cached device info (fast, but may have -1 for channel counts)
 * @param deviceIndex Index of the device
 * @param nameBuffer Buffer to receive device name (minimum 256 bytes)
 * @param typeNameBuffer Buffer to receive device type (minimum 64 bytes)
 * @param maxInputs Pointer to receive max input channels (-1 if unknown)
 * @param maxOutputs Pointer to receive max output channels (-1 if unknown)
 * @return AUDIO_PLUGIN_OK if successful, AUDIO_PLUGIN_ERROR otherwise
 */
EXPORT_API int CALL_CONV AT_WS_getCachedDeviceInfo(int deviceIndex, char* nameBuffer, char* typeNameBuffer, int* maxInputs, int* maxOutputs);

/**
 * @brief Get detailed device info (slow, creates device temporarily)
 * @param deviceIndex Index of the device
 * @param nameBuffer Buffer to receive device name (minimum 256 bytes)
 * @param typeNameBuffer Buffer to receive device type (minimum 64 bytes)
 * @param maxInputs Pointer to receive max input channels
 * @param maxOutputs Pointer to receive max output channels
 * @return AUDIO_PLUGIN_OK if successful, AUDIO_PLUGIN_ERROR otherwise
 */
EXPORT_API int CALL_CONV AT_WS_getDetailedDeviceInfo(int deviceIndex, char* nameBuffer, char* typeNameBuffer, int* maxInputs, int* maxOutputs);


/**
 * @briefValidate all cached devices and filter out unavailable ones
 * Devices that cannot be created (not connected) are marked with 0 channels
 * Call this after device scan completes
 */
EXPORT_API void CALL_CONV AT_WS_filterUnavailableDevices();

// ============================================================================
// BINAURAL VIRTUALIZATION OF SPEAKERS
// ============================================================================

/**
 * @brief Load HRTF data from a text file for binaural spatialization
 * 
 * This function loads HRTF (Head-Related Transfer Function) data from a text file
 * in the format generated by SOFA converter tools. The HRTF data will be loaded
 * into all HRTF processors for binaural virtualization.
 * 
 * The file format should be:
 * HEADER <sampleRate> <irLength>
 * <azimuth> <elevation> [distance] <leftIR_samples...> <rightIR_samples...>
 * ...
 * 
 * @param filePath Full path to the HRTF data file (.txt format)
 * @return AUDIO_PLUGIN_OK if successful, AUDIO_PLUGIN_ERROR otherwise
 * 
 * Note: Binaural virtualization must be enabled before loading HRTF data.
 *       Call AT_WS_setIsBinauralVirtualization(true) first.
 */
EXPORT_API int CALL_CONV AT_WS_loadHRTF(const char* filePath);

/**
 * @brief Load default built-in HRTF for binaural spatialization
 * 
 * Loads a simple generic HRTF that is built into the engine. This is useful
 * as a fallback when no custom HRTF file is available. The default HRTF
 * provides basic spatial cues (ITD and ILD) for common positions.
 * 
 * @return AUDIO_PLUGIN_OK if successful, AUDIO_PLUGIN_ERROR otherwise
 * 
 * Note: Binaural virtualization must be enabled before loading HRTF data.
 *       Call AT_WS_setIsBinauralVirtualization(true) first.
 */
EXPORT_API int CALL_CONV AT_WS_loadDefaultHRTF();

/**
 * @brief Set simple binaural spatialization mode for A/B comparison
 *
 * Enables or disables simple binaural mode, which bypasses WFS and applies
 * HRTF directly to the mono source. This allows for runtime A/B comparison
 * between WFS + binaural virtualization and simple binaural panning.
 *
 * IMPORTANT CONSTRAINTS:
 * - Can only be enabled if binaural virtualization is active (m_isBinauralVirtualization = true)
 * - Uses the same HRTF processors as WFS binaural virtualization
 * - Can be toggled at runtime for A/B comparison
 * - Output is always stereo (2 channels) in either mode
 *
 * @param isSimpleBinauralSpat True to enable simple binaural mode, false for WFS + binaural mode
 * @return AUDIO_PLUGIN_OK if mode was changed successfully, AUDIO_PLUGIN_ERROR if rejected
 *         (e.g., binaural virtualization not enabled)
 */
EXPORT_API int CALL_CONV AT_WS_setIsSimpleBinauralSpat(bool isSimpleBinauralSpat);


/**
 * @brief Enables or disables the near-field ILD correction on binaural output.
 *
 * When enabled, a first-order stereo shelving filter (rigid sphere model, Duda & Martens
 * 1998) is applied to the accumulated binaural buffer. It restores the low-frequency
 * ILD that is present in the real WFS field but absent when the binaural
 * engine uses a distance-independent BRIR set.
 *
 * Has no effect unless binaural virtualization is active
 * (AT_WS_setup called with isBinauralVirtualization=true).
 *
 * @param isEnabled  true = correction active, false = bypass (transparent).
 * @return AUDIO_PLUGIN_OK if successful, AUDIO_PLUGIN_ERROR otherwise.
 *
 * Example C# usage:
 * @code
 * [DllImport("AT_WS_AudioEngineLib")]
 * private static extern int AT_WS_setIsNearFieldCorrection(bool isEnabled);
 *
 * AT_WS_setIsNearFieldCorrection(true);
 * @endcode
 */
EXPORT_API int CALL_CONV AT_WS_setIsNearFieldCorrection(bool isEnabled);

/**
 * @brief Enables or disables HRTF IR truncation to 512 samples.
 *
 * When enabled, every HRTF impulse response loaded by the convolution engine
 * is cut to 512 samples. This halves (or more) the FFT cost per convolver and
 * is particularly effective at high virtual-speaker counts (≥ 64 channels).
 * The truncation trades minor low-frequency accuracy for a significant CPU
 * reduction. Disable to restore the full IR length from the HRTF file.
 *
 * C# usage:
 * @code
 * [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)]
 * private static extern int AT_WS_setHrtfTruncate(bool enabled);
 *
 * AT_WS_setHrtfTruncate(true);
 * @endcode
 */
EXPORT_API int CALL_CONV AT_WS_setHrtfTruncate(bool enabled);

/**
 * @brief Sets the HRTF reference distance for the near-field ILD correction.
 *
 * Call this once at setup time after loading the HRTF dataset. This is the
 * only NFC parameter that cannot be inferred from scene geometry — it is a
 * property of the HRTF/BRIR set (the distance at which IRs were measured).
 *
 * rVirtual and azimuthDeg are recomputed automatically by the engine on
 * every AT_WS_setListenerTransform() and AT_WS_setPlayerTransform() call,
 * so no further NFC geometry update is needed from the caller.
 *
 * @param rRef        HRTF measurement distance (m). Typically 1.0 m.
 * @param headRadius  Sphere radius in metres (0 = use KEMAR default 0.0875 m).
 * @return AUDIO_PLUGIN_OK if successful, AUDIO_PLUGIN_ERROR otherwise.
 */
EXPORT_API int CALL_CONV AT_WS_setNearFieldCorrectionRRef(float rRef,
                                                           float headRadius);


#ifdef __cplusplus
}
#endif
