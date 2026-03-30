/**
 * @file PluginInterface.cpp
 * @brief Implementation of the C interface for the audio library
 * @author Antoine Gonot
 * @date 2026
 */

#include "PluginInterface.h"
#include "AT_AudioManager.h"
#include <mutex>
#include <memory>
#include "UnityLogger.h"

// ============================================================================
// GLOBAL VARIABLES
// ============================================================================

static std::unique_ptr<AT::AudioManager> g_audioManager = nullptr;
static std::mutex g_mutex;

// Callback for Unity logging
static void (*g_logCallback)(const char*) = nullptr;

// ============================================================================
// HELPERS
// ============================================================================

/**
 * @brief Checks that the manager is initialized
 * @return true if initialized
 */
static bool IsManagerValid()
{
    return g_audioManager != nullptr;
}

/**
 * @brief Gets the spatialization engine
 * @return Pointer to the engine, or nullptr
 */
static AT::SpatializationEngine* GetEngine()
{
    if (!IsManagerValid())
        return nullptr;
    return g_audioManager->getSpatializationEngine();
}

// ============================================================================
// LOGGING
// ============================================================================

/**
 * @brief Sets the logging callback for Unity
 * @param callback Callback function to receive logs
 * 
 * This function allows Unity to receive log messages from the plugin.
 * The callback will be called with C strings (const char*) for each message.
 */
EXPORT_API void CALL_CONV AT_WS_setLogCallback(void (*callback)(const char*))
{
    std::lock_guard<std::mutex> lock(g_mutex);
    g_logCallback = callback;
    
    // Send a test message if callback is set
    if (g_logCallback)
    {
        g_logCallback("[AT_SPAT] Log callback registered successfully");
    }
}

// ============================================================================
// PLUGIN MANAGEMENT
// ============================================================================

EXPORT_API int CALL_CONV AT_WS_initialize()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    
    if (g_audioManager != nullptr)
    {
        return AUDIO_PLUGIN_OK; // Already initialized
    }
    
    try
    {
        // Create the AudioManager
        // Constructor initializes JUCE and scans devices
        g_audioManager = std::make_unique<AT::AudioManager>();
        return AUDIO_PLUGIN_OK;
    }
    catch (...)
    {
        g_audioManager.reset();
        return AUDIO_PLUGIN_ERROR;
    }
}

EXPORT_API int CALL_CONV AT_WS_setup(const char* audioDeviceName, int inputChannels, int outputChannels, int bufferSize, bool isBinauralVirtualization)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    
    if (!IsManagerValid())
    {
        return AUDIO_PLUGIN_ERROR;
    }

    // =========================================================================
    // GUARD: reject channel counts that exceed the pre-allocated maximum.
    //
    // All per-channel arrays in AT_Spatializer and AT_SpatializationEngine are
    // statically sized to MAX_VIRTUAL_SPEAKERS at compile time. Exceeding this
    // limit would cause out-of-bounds writes and undefined behaviour.
    //
    // The Unity integration mirrors this constant as At_MasterOutput.MAX_VIRTUAL_SPEAKERS
    // so the error should normally be caught on the C# side first, but this
    // check is the authoritative safety net at the library boundary.
    // =========================================================================
    if (outputChannels > AT::SpatializationEngine::MAX_VIRTUAL_SPEAKERS)
    {
        LOG_ERROR("AT_WS_setup: requested outputChannels (" << outputChannels
            << ") exceeds MAX_VIRTUAL_SPEAKERS (" << AT::SpatializationEngine::MAX_VIRTUAL_SPEAKERS
            << "). Reduce the WFS channel count or rebuild the library with a larger limit.");
        return AUDIO_PLUGIN_ERROR;
    }
    
    std::string deviceName = (audioDeviceName != nullptr) ? audioDeviceName : "";
    
    // Delegate to manager.  setup() now returns false when the channel count guard
    // fires (non-binaural mode: requested physical outputs > device maximum) or when
    // SpatializationEngine::setup() throws.  Either condition is fatal at this stage
    // — the audio device is not open and no audio processing can take place.
    bool ok = g_audioManager->setup(deviceName, inputChannels, outputChannels,
                                    bufferSize, isBinauralVirtualization);
    if (!ok)
    {
        LOG_ERROR("AT_WS_setup: setup failed for device '" << deviceName
            << "' (" << outputChannels << " virtual speaker(s), "
            << "binaural=" << (isBinauralVirtualization ? "true" : "false") << ").");
        return AUDIO_PLUGIN_ERROR;
    }
    return AUDIO_PLUGIN_OK;
}

EXPORT_API int CALL_CONV AT_WS_stop()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    
    if (!IsManagerValid())
    {
        return AUDIO_PLUGIN_ERROR;
    }
    
    g_audioManager->stop();
    return AUDIO_PLUGIN_OK;
}

EXPORT_API int CALL_CONV AT_WS_shutdown()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    
    if (g_audioManager)
    {
        g_audioManager.reset();
    }
    
    return AUDIO_PLUGIN_OK;
}

// ============================================================================
// AUDIO DEVICE ENUMERATION
// ============================================================================

EXPORT_API int CALL_CONV AT_WS_getDeviceCount()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    
    if (!IsManagerValid())
    {
        return 0;
    }
    
    return g_audioManager->getCachedDeviceCount();
}

EXPORT_API int CALL_CONV AT_WS_getDeviceName(int deviceIndex, char* deviceName)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    
    if (!IsManagerValid() || deviceName == nullptr)
    {
        return AUDIO_PLUGIN_ERROR;
    }
    
    AT::DeviceInfo info = g_audioManager->getCachedDeviceInfo(deviceIndex);
    
    if (info.name.empty())
    {
        deviceName[0] = '\0';
        return AUDIO_PLUGIN_ERROR;
    }
    
    // Copy name (max 256 characters)
    strncpy(deviceName, info.name.c_str(), 255);
    deviceName[255] = '\0';
    
    return AUDIO_PLUGIN_OK;
}

EXPORT_API int CALL_CONV AT_WS_getDeviceChannels(int deviceIndex, int isOutput)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    
    if (!IsManagerValid())
    {
        return -1;
    }
    
    AT::DeviceInfo info = g_audioManager->getCachedDeviceInfo(deviceIndex);
    
    if (info.name.empty())
    {
        return -1;
    }
    
    return (isOutput != 0) ? info.maxOutputChannels : info.maxInputChannels;
}

// ============================================================================
// PLAYER MANAGEMENT
// ============================================================================

EXPORT_API int CALL_CONV AT_WS_addPlayer(int* uid, bool is3D, bool isLooping)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    
    if (!IsManagerValid() || uid == nullptr)
    {
        return AUDIO_PLUGIN_ERROR;
    }
    
    // addPlayer fills uid by pointer (void)
    g_audioManager->addPlayer(uid, is3D, isLooping);
    return (*uid >= 0) ? AUDIO_PLUGIN_OK : AUDIO_PLUGIN_ERROR;
}

EXPORT_API int CALL_CONV AT_WS_removePlayer(int uid)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    
    if (!IsManagerValid())
    {
        return AUDIO_PLUGIN_ERROR;
    }
    
    // removePlayer returns bool
    return g_audioManager->removePlayer(uid) ? AUDIO_PLUGIN_OK : AUDIO_PLUGIN_ERROR;
}

EXPORT_API int CALL_CONV AT_WS_setPlayerFilePath(int uid, const char* filepath)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    
    if (!IsManagerValid() || filepath == nullptr)
    {
        return AUDIO_PLUGIN_ERROR;
    }
    
    // setPlayerFilePath returns bool
    return g_audioManager->setPlayerFilePath(uid, filepath) ? AUDIO_PLUGIN_OK : AUDIO_PLUGIN_ERROR;
}

EXPORT_API int CALL_CONV AT_WS_getAudioFileMetadata (
    const char* filepath,
    int* numChannels,
    double* sampleRate,
    double* lengthSeconds,
    long long* totalSamples)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    
    // Check parameters (no need for manager to read a file)
    if (filepath == nullptr || 
        numChannels == nullptr || sampleRate == nullptr || 
        lengthSeconds == nullptr || totalSamples == nullptr)
    {
        return AUDIO_PLUGIN_ERROR;
    }
    
    try
    {
        // Check that JUCE is initialized
        // NOTE: g_audioManager must be created (AT_WS_initialize called) for JUCE to be initialized
        if (g_audioManager == nullptr)
        {
            // JUCE is not initialized - cannot read files
            // User must call AT_WS_initialize() first
            *numChannels = 0;
            *sampleRate = 0.0;
            *lengthSeconds = 0.0;
            *totalSamples = 0;
            return AUDIO_PLUGIN_ERROR;
        }
        
        // Create a local format manager to read audio files
        juce::AudioFormatManager formatManager;
        formatManager.registerBasicFormats(); // WAV, AIFF, FLAC, OGG, MP3
        
        // Create a JUCE file
        juce::File audioFile(filepath);
        
        if (!audioFile.existsAsFile())
        {
            // Log: File not found
            juce::String msg = "AT_WS_getAudioFileMetadata - File not found: " + juce::String(filepath);
            juce::Logger::writeToLog(msg);
            
            *numChannels = 0;
            *sampleRate = 0.0;
            *lengthSeconds = 0.0;
            *totalSamples = 0;
            return AUDIO_PLUGIN_ERROR;
        }
        
        // Create a reader for the file
        std::unique_ptr<juce::AudioFormatReader> reader(
            formatManager.createReaderFor(audioFile)
        );
        
        if (reader == nullptr)
        {
            // Log: Unable to read file
            juce::String msg = "AT_WS_getAudioFileMetadata - Unable to read file: " + juce::String(filepath);
            juce::Logger::writeToLog(msg);
            
            *numChannels = 0;
            *sampleRate = 0.0;
            *lengthSeconds = 0.0;
            *totalSamples = 0;
            return AUDIO_PLUGIN_ERROR;
        }
        
        // Extract metadata from the reader
        *numChannels = static_cast<int>(reader->numChannels);
        *sampleRate = reader->sampleRate;
        *totalSamples = reader->lengthInSamples;
        
        // Calculate duration in seconds
        if (*sampleRate > 0.0)
        {
            *lengthSeconds = static_cast<double>(*totalSamples) / (*sampleRate);
        }
        else
        {
            *lengthSeconds = 0.0;
        }
        
        return AUDIO_PLUGIN_OK;
    }
    catch (...)
    {
        *numChannels = 0;
        *sampleRate = 0.0;
        *lengthSeconds = 0.0;
        *totalSamples = 0;
        return AUDIO_PLUGIN_ERROR;
    }
}

// ============================================================================
// PLAYBACK CONTROL
// ============================================================================

EXPORT_API int CALL_CONV AT_WS_startPlayer(int uid)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    
    if (!IsManagerValid())
    {
        return AUDIO_PLUGIN_ERROR;
    }
    
    g_audioManager->startPlayer(uid);
    return AUDIO_PLUGIN_OK;
}

EXPORT_API int CALL_CONV AT_WS_stopPlayer(int uid)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    
    if (!IsManagerValid())
    {
        return AUDIO_PLUGIN_ERROR;
    }
    
    g_audioManager->stopPlayer(uid);
    return AUDIO_PLUGIN_OK;
}

EXPORT_API int CALL_CONV AT_WS_startAllPlayers()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    
    if (!IsManagerValid())
    {
        return AUDIO_PLUGIN_ERROR;
    }
    
    g_audioManager->startAllPlayers();
    return AUDIO_PLUGIN_OK;
}

EXPORT_API int CALL_CONV AT_WS_stopAllPlayers()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    
    if (!IsManagerValid())
    {
        return AUDIO_PLUGIN_ERROR;
    }
    
    g_audioManager->stopAllPlayers();
    return AUDIO_PLUGIN_OK;
}

EXPORT_API int CALL_CONV AT_WS_setIsPrefilterAllPlayers(bool isPrefilter)
{
    if (g_audioManager == nullptr) return AUDIO_PLUGIN_ERROR;
    g_audioManager->setIsPrefilterAllPlayers(isPrefilter);
    return AUDIO_PLUGIN_OK;
}

// ============================================================================
// PLAYER PARAMETERS
// ============================================================================

EXPORT_API int CALL_CONV AT_WS_setPlayerTransform(int uid, float* position, float* rotation, float* forward)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    
    if (!IsManagerValid() || position == nullptr || rotation == nullptr || forward == nullptr)
    {
        return AUDIO_PLUGIN_ERROR;
    }
    
    g_audioManager->setPlayerTransform(uid, position, rotation, forward);
    return AUDIO_PLUGIN_OK;
}

EXPORT_API int CALL_CONV AT_WS_setPlayerParams(int uid, float gain, float playbackSpeed, 
                                                  float attenuation, float minDistance)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    
    if (!IsManagerValid())
    {
        return AUDIO_PLUGIN_ERROR;
    }
    
    g_audioManager->setPlayerRealTimeParameter(uid, gain, playbackSpeed, attenuation, minDistance);
    return AUDIO_PLUGIN_OK;
}


EXPORT_API int CALL_CONV AT_WS_enableAllPlayersSpeakerMask(bool isWfsSpeakerMask)
{
    if (g_audioManager == nullptr) return AUDIO_PLUGIN_ERROR;
    g_audioManager->enableAllPlayersSpeakerMask(isWfsSpeakerMask);
    return AUDIO_PLUGIN_OK;
}

EXPORT_API int CALL_CONV AT_WS_setIsWfsGain(bool isWfsGain)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    if (!IsManagerValid())
        return AUDIO_PLUGIN_ERROR;

    g_audioManager->setIsWfsGain(isWfsGain);

    LOG("[PluginInterface] WFS gain " << (isWfsGain ? "ENABLED (cos(phi)/sqrt(r))" : "DISABLED (unity gain)"));

    return AUDIO_PLUGIN_OK;
}

EXPORT_API int CALL_CONV AT_WS_setIsActiveSpeakersMinMax(bool enabled)
{
    if (g_audioManager == nullptr) return AUDIO_PLUGIN_ERROR;
    g_audioManager->setIsActiveSpeakersMinMax(enabled);
    return AUDIO_PLUGIN_OK;
}

EXPORT_API int CALL_CONV AT_WS_setSecondarySourceSize(float secondarySourceSize)
{
    if (g_audioManager == nullptr) return AUDIO_PLUGIN_ERROR;
    g_audioManager->setSecondarySourceSize(secondarySourceSize);
    LOG("[PluginInterface] Source size = " << secondarySourceSize
        << " m  (P1: r_amp=sqrt(r2+eps2), P2: mask taper width=" << secondarySourceSize << " m)");
    return AUDIO_PLUGIN_OK;
}

/**
 * @brief Alias for AT_WS_setPlayerParams (compatibility)
 * 
 * This function is identical to AT_WS_setPlayerParams.
 * It exists for compatibility with the old function name.
 */
EXPORT_API int CALL_CONV AT_WS_setPlayerRealTimeParameter(int uid, float gain, float playbackSpeed, 
                                                             float attenuation, float minDistance)
{
    // Delegate to AT_WS_setPlayerParams
    return AT_WS_setPlayerParams(uid, gain, playbackSpeed, attenuation, minDistance);
}

// ============================================================================
// INFORMATION RETRIEVAL
// ============================================================================

EXPORT_API int CALL_CONV AT_WS_getPlayerWfsDelay(int uid, float* delay, int arraySize)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    try
    {
        if (!g_audioManager)
        {
            // Not an error - might be called after shutdown
            return AUDIO_PLUGIN_OK;
        }

        // Delegate to AudioManager
        g_audioManager->getPlayerWfsDelay(uid, delay, arraySize);

        return AUDIO_PLUGIN_OK;
    }
    catch (...)
    {
        return AUDIO_PLUGIN_ERROR;
    }
}

EXPORT_API int CALL_CONV AT_WS_getPlayerWfsLinGain(int uid, float* linGain, int arraySize)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    try
    {
        if (!g_audioManager)
        {
            // Not an error - might be called after shutdown
            return AUDIO_PLUGIN_OK;
        }

        // Delegate to AudioManager
        g_audioManager->getPlayerWfsLinGain(uid, linGain, arraySize);

        return AUDIO_PLUGIN_OK;
    }
    catch (...)
    {
        return AUDIO_PLUGIN_ERROR;
    }
}

EXPORT_API int CALL_CONV AT_WS_getPlayerMeters(int uid, float* meters, int arraySize)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    
    if (!IsManagerValid() || meters == nullptr || arraySize <= 0)
    {
        return AUDIO_PLUGIN_ERROR;
    }
    
    g_audioManager->getPlayerMeters(uid, meters, arraySize);
    return AUDIO_PLUGIN_OK;
}

EXPORT_API int CALL_CONV AT_WS_getPlayerNumChannel(int uid, int* numChannel)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    
    if (!IsManagerValid() || numChannel == nullptr)
    {
        return AUDIO_PLUGIN_ERROR;
    }
    
    g_audioManager->getPlayerNumChannel(uid, numChannel);
    return AUDIO_PLUGIN_OK;
}

EXPORT_API int CALL_CONV AT_WS_getPlayerSpeakerMask(int uid, float* speakerMask, int arraySize)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    try
    {
        if (!g_audioManager)
        {
            // Not an error - might be called after shutdown
            return AUDIO_PLUGIN_OK;
        }

        // Delegate to AudioManager
        g_audioManager->getPlayerSpeakerMask(uid, speakerMask, arraySize);

        return AUDIO_PLUGIN_OK;
    }
    catch (...)
    {
        return AUDIO_PLUGIN_ERROR;
    }
}

EXPORT_API int CALL_CONV AT_WS_getMixerOutputMeters(float* meters, int arraySize)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    
    if (!IsManagerValid() || meters == nullptr || arraySize <= 0)
    {
        return AUDIO_PLUGIN_ERROR;
    }
    
    g_audioManager->getMixerOutputMeters(meters, arraySize);
    return AUDIO_PLUGIN_OK;
}

// ============================================================================
// GLOBAL PARAMETERS
// ============================================================================

EXPORT_API int CALL_CONV AT_WS_setListenerTransform(float* position, float* rotation, float* forward)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    
    if (!IsManagerValid() || position == nullptr || rotation == nullptr || forward == nullptr)
    {
        return AUDIO_PLUGIN_ERROR;
    }
    
    g_audioManager->setListenerTransform(position, rotation, forward);
    return AUDIO_PLUGIN_OK;
}

EXPORT_API int CALL_CONV AT_WS_setVirtualSpeakerTransform(float* positions, float* rotations, float* forwards, int virtualSpeakerCount)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    try
    {
        if (!g_audioManager)
        {
            // Not an error - might be called after shutdown
            return AUDIO_PLUGIN_OK;
        }

        // Delegate to AudioManager
        g_audioManager->setVirtualSpeakerTransform(positions, rotations, forwards, virtualSpeakerCount);

        return AUDIO_PLUGIN_OK;
    }
    catch (...)
    {
        return AUDIO_PLUGIN_ERROR;
    }
}

EXPORT_API int CALL_CONV AT_WS_setMaxDistanceForDelay(float maxDistance)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    try
    {
        if (!g_audioManager)
        {
            // Not an error - might be called after shutdown
            return AUDIO_PLUGIN_OK;
        }

        // Delegate to AudioManager
        g_audioManager->setMaxDistanceForDelay(maxDistance);

        return AUDIO_PLUGIN_OK;
    }
    catch (...)
    {
        return AUDIO_PLUGIN_ERROR;
    }
}

EXPORT_API int CALL_CONV AT_WS_setMasterGain(float masterGain)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    
    if (!IsManagerValid())
    {
        return AUDIO_PLUGIN_ERROR;
    }
    
    g_audioManager->setMasterGain(masterGain);
    return AUDIO_PLUGIN_OK;
}
EXPORT_API int CALL_CONV AT_WS_setMakeupMasterGain(float makeupMasterGain)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    
    if (!IsManagerValid())
    {
        return AUDIO_PLUGIN_ERROR;
    }
    
    g_audioManager->setMakeupMasterGain(makeupMasterGain);
    return AUDIO_PLUGIN_OK;
}
// ============================================================================
// OPTIMIZED DEVICE SCANNING (NEW FUNCTIONS)
// ============================================================================

EXPORT_API int CALL_CONV AT_WS_isDeviceScanComplete()
{
    if (!IsManagerValid())
    {
        return 0; // Not initialized, scan not complete
    }
    
    try
    {
        return g_audioManager->isDeviceScanComplete() ? 1 : 0;
    }
    catch (...)
    {
        return 0;
    }
}

EXPORT_API int CALL_CONV AT_WS_waitForDeviceScan(int timeoutMs)
{
    if (!IsManagerValid())
    {
        return 0; // Error/timeout
    }
    
    try
    {
        return g_audioManager->waitForDeviceScan(timeoutMs) ? 1 : 0;
    }
    catch (...)
    {
        return 0; // Error/timeout
    }
}

EXPORT_API void CALL_CONV AT_WS_refreshDevices(int includeASIO, int async)
{
    if (!IsManagerValid())
    {
        return;
    }
    
    try
    {
        g_audioManager->refreshDevices(includeASIO != 0, async != 0);
    }
    catch (...)
    {
        // Error, but void function - just return
    }
}

EXPORT_API int CALL_CONV AT_WS_getCachedDeviceInfo(int deviceIndex, char* nameBuffer, char* typeNameBuffer, int* maxInputs, int* maxOutputs)
{
    if (!IsManagerValid())
    {
        return AUDIO_PLUGIN_ERROR;
    }
    
    if (nameBuffer == nullptr || typeNameBuffer == nullptr || maxInputs == nullptr || maxOutputs == nullptr)
    {
        return AUDIO_PLUGIN_ERROR;
    }
    
    try
    {
        AT::DeviceInfo info = g_audioManager->getCachedDeviceInfo(deviceIndex);
        
        if (info.name.empty())
        {
            return AUDIO_PLUGIN_ERROR;
        }
        
        // Copy device name and type to buffers
        std::strncpy(nameBuffer, info.name.c_str(), 255);
        nameBuffer[255] = '\0';
        
        std::strncpy(typeNameBuffer, info.typeName.c_str(), 63);
        typeNameBuffer[63] = '\0';
        
        *maxInputs = info.maxInputChannels;
        *maxOutputs = info.maxOutputChannels;
        
        return AUDIO_PLUGIN_OK;
    }
    catch (...)
    {
        return AUDIO_PLUGIN_ERROR;
    }
}

EXPORT_API int CALL_CONV AT_WS_getDetailedDeviceInfo(int deviceIndex, char* nameBuffer, char* typeNameBuffer, int* maxInputs, int* maxOutputs)
{
    if (!IsManagerValid())
    {
        return AUDIO_PLUGIN_ERROR;
    }
    
    if (nameBuffer == nullptr || typeNameBuffer == nullptr || maxInputs == nullptr || maxOutputs == nullptr)
    {
        return AUDIO_PLUGIN_ERROR;
    }
    
    try
    {
        AT::DeviceInfo info = g_audioManager->getDetailedDeviceInfo(deviceIndex);
        
        if (info.name.empty())
        {
            return AUDIO_PLUGIN_ERROR;
        }
        
        // Copy device name and type to buffers
        std::strncpy(nameBuffer, info.name.c_str(), 255);
        nameBuffer[255] = '\0';
        
        std::strncpy(typeNameBuffer, info.typeName.c_str(), 63);
        typeNameBuffer[63] = '\0';
        
        *maxInputs = info.maxInputChannels;
        *maxOutputs = info.maxOutputChannels;
        
        return AUDIO_PLUGIN_OK;
    }
    catch (...)
    {
        return AUDIO_PLUGIN_ERROR;
    }
}

EXPORT_API void CALL_CONV AT_WS_filterUnavailableDevices()
{
    if (!IsManagerValid())
    {
        LOG_ERROR("[PluginInterface] AT_WS_filterUnavailableDevices: Manager not initialized");
        return;
    }

    try
    {
        g_audioManager->filterUnavailableDevices();
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("[PluginInterface] AT_WS_filterUnavailableDevices exception: " << e.what());
    }
    catch (...)
    {
        LOG_ERROR("[PluginInterface] AT_WS_filterUnavailableDevices unknown exception");
    }
}


// ============================================================================
// BINAURAL VIRTUALIZATION OF SPEAKERS
// ============================================================================


EXPORT_API int CALL_CONV AT_WS_loadHRTF(const char* filePath)
{
    std::lock_guard<std::mutex> lock(g_mutex);
    
    if (!IsManagerValid())
    {
        LOG_ERROR("[PluginInterface] AT_WS_loadHRTF: AudioManager not initialized");
        return AUDIO_PLUGIN_ERROR;
    }
    
    if (!filePath || strlen(filePath) == 0)
    {
        LOG_ERROR("[PluginInterface] AT_WS_loadHRTF: File path is null or empty");
        return AUDIO_PLUGIN_ERROR;
    }
    
    LOG("[PluginInterface] Loading HRTF from file: " << filePath);
    
    try
    {
        auto* engine = GetEngine();
        if (!engine)
        {
            LOG_ERROR("[PluginInterface] AT_WS_loadHRTF: Failed to get engine");
            return AUDIO_PLUGIN_ERROR;
        }
        
        bool success = engine->loadHRTFFile(std::string(filePath));
        
        if (success)
        {
            LOG("[PluginInterface] HRTF file loaded successfully: " << filePath);
            return AUDIO_PLUGIN_OK;
        }
        else
        {
            LOG_ERROR("[PluginInterface] Failed to load HRTF file: " << filePath);
            return AUDIO_PLUGIN_ERROR;
        }
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("[PluginInterface] Exception while loading HRTF: " << e.what());
        return AUDIO_PLUGIN_ERROR;
    }
    catch (...)
    {
        LOG_ERROR("[PluginInterface] Unknown exception while loading HRTF");
        return AUDIO_PLUGIN_ERROR;
    }
}

EXPORT_API int CALL_CONV AT_WS_loadDefaultHRTF()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    
    if (!IsManagerValid())
    {
        LOG_ERROR("[PluginInterface] AT_WS_loadDefaultHRTF: AudioManager not initialized");
        return AUDIO_PLUGIN_ERROR;
    }
    
    LOG("[PluginInterface] Loading default built-in HRTF");
    
    try
    {
        auto* engine = GetEngine();
        if (!engine)
        {
            LOG_ERROR("[PluginInterface] AT_WS_loadDefaultHRTF: Failed to get engine");
            return AUDIO_PLUGIN_ERROR;
        }
        
        bool success = engine->loadDefaultHRTF();
        
        if (success)
        {
            LOG("[PluginInterface] Default HRTF loaded successfully");
            return AUDIO_PLUGIN_OK;
        }
        else
        {
            LOG_ERROR("[PluginInterface] Failed to load default HRTF");
            return AUDIO_PLUGIN_ERROR;
        }
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("[PluginInterface] Exception while loading default HRTF: " << e.what());
        return AUDIO_PLUGIN_ERROR;
    }
    catch (...)
    {
        LOG_ERROR("[PluginInterface] Unknown exception while loading default HRTF");
        return AUDIO_PLUGIN_ERROR;
    }
}

EXPORT_API int CALL_CONV AT_WS_setIsSimpleBinauralSpat(bool isSimpleBinauralSpat)
{
    try
    {
        auto* engine = GetEngine();
        if (!engine)
        {
            LOG_ERROR("[PluginInterface] AT_WS_setIsSimpleBinauralSpat: Failed to get engine");
            return AUDIO_PLUGIN_ERROR;
        }
        
        bool success = engine->setIsSimpleBinauralSpat(isSimpleBinauralSpat);
        
        if (success)
        {
            if (isSimpleBinauralSpat)
            {
                LOG("[PluginInterface] Simple binaural mode ENABLED");
            }
            else
            {
                LOG("[PluginInterface] Simple binaural mode DISABLED (using WFS + binaural)");
            }
            return AUDIO_PLUGIN_OK;
        }
        else
        {
            LOG_ERROR("[PluginInterface] Failed to set simple binaural mode");
            LOG_ERROR("[PluginInterface] Binaural virtualization must be enabled first");
            return AUDIO_PLUGIN_ERROR;
        }
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("[PluginInterface] Exception while setting simple binaural mode: " << e.what());
        return AUDIO_PLUGIN_ERROR;
    }
    catch (...)
    {
        LOG_ERROR("[PluginInterface] Unknown exception while setting simple binaural mode");
        return AUDIO_PLUGIN_ERROR;
    }
}

EXPORT_API int CALL_CONV AT_WS_setIsNearFieldCorrection(bool isEnabled)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    auto* engine = GetEngine();
    if (!engine)
    {
        LOG_ERROR("[PluginInterface] AT_WS_setIsNearFieldCorrection: Failed to get engine");
        return AUDIO_PLUGIN_ERROR;
    }

    engine->setIsNearFieldCorrection(isEnabled);
    LOG("[PluginInterface] Near-field correction " << (isEnabled ? "ENABLED" : "DISABLED"));
    return AUDIO_PLUGIN_OK;
}

EXPORT_API int CALL_CONV AT_WS_setHrtfTruncate(bool enabled)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    auto* engine = GetEngine();
    if (!engine)
    {
        LOG_ERROR("[PluginInterface] AT_WS_setHrtfTruncate: Failed to get engine");
        return AUDIO_PLUGIN_ERROR;
    }

    engine->setHrtfTruncate(enabled);
    LOG("[PluginInterface] HRTF truncation " << (enabled ? "ENABLED (512 samples)" : "DISABLED (full IR)"));
    return AUDIO_PLUGIN_OK;
}

EXPORT_API int CALL_CONV AT_WS_setNearFieldCorrectionRRef(float rRef, float headRadius)
{
    std::lock_guard<std::mutex> lock(g_mutex);

    auto* engine = GetEngine();
    if (!engine)
    {
        LOG_ERROR("[PluginInterface] AT_WS_setNearFieldCorrectionRRef: Failed to get engine");
        return AUDIO_PLUGIN_ERROR;
    }

    // headRadius == 0.0f is the "use default" sentinel
    const float radius = (headRadius <= 0.0f)
        ? AT::NearFieldCorrection::DEFAULT_HEAD_RADIUS
        : headRadius;

    engine->setNearFieldCorrectionRRef(rRef, radius);
    return AUDIO_PLUGIN_OK;
}
