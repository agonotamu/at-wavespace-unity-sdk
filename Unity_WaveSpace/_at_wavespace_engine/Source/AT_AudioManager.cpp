/**
 * @file AT_AudioManager.cpp
 * @brief Implementation of the main audio manager
 * @author Antoine Gonot
 * @date 2026
 */

#include "AT_AudioManager.h"
#include "UnityLogger.h"
#include <cstring>
#include <chrono>
#include "AT_SpatConfig.h"

namespace AT
{
    // ========================================================================
    // CONSTRUCTOR / DESTRUCTOR
    // ========================================================================

    AudioManager::AudioManager()
        : m_devicesCached(false)
        , m_deviceSampleRate(0)
        , m_isPlayerActive(false)
    {
        // Register ourselves as listener for the spatialization engine
        m_spatializationEngine.setListener(this);

        // IMPORTANT: Do NOT scan devices here in the constructor.
        // Console applications have no JUCE message loop, so async scanning
        // will crash with JUCE_ASSERT_MESSAGE_THREAD.
        //
        // Console apps MUST call refreshDevices() manually:
        //   refreshDevices(includeASIO, false)  — synchronous scan
        //   Never use async = true in console applications.

#ifndef AT_SPAT_CONSOLE_APP
        #if JUCE_WINDOWS
           refreshDevices(true, true);   // async with ASIO
        #else
           refreshDevices(false, true);  // async without ASIO
        #endif

        LOG("[AudioManager] Created. Call refreshDevices(includeASIO, false) to scan devices.");
#endif
    }

    AudioManager::~AudioManager()
    {
        // Signal scan thread to stop
        m_shouldStopScan = true;

        // Wait for scan thread to complete (with timeout)
        if (m_scanThread.joinable())
        {
            try
            {
                m_scanThread.join();
            }
            catch (...)
            {
                LOG_WARNING("[AudioManager] Exception while joining scan thread");
            }
        }

        // Clean audio shutdown
        stop();
    }

    // ========================================================================
    // AudioManagerListener INTERFACE
    // ========================================================================

    void AudioManager::prepareToPlay(int samplesPerBlock, double sampleRate)
    {
        m_deviceSampleRate = sampleRate;
    }

    void AudioManager::releaseResources()
    {
        // No-op — can be extended if needed
    }

    // ========================================================================
    // DEVICE ENUMERATION
    // ========================================================================

    void AudioManager::scanAndCacheDevices(bool includeASIO)
    {
        LOG("[AudioManager] scanAndCacheDevices() called (includeASIO="
            << (includeASIO ? "true" : "false") << ")");

        auto startTime = std::chrono::high_resolution_clock::now();

        std::lock_guard<std::mutex> lock(m_deviceCacheMutex);
        m_cachedDevices.clear();

        try
        {
            juce::AudioDeviceManager tempDeviceManager;

            #if JUCE_WINDOWS
            juce::OwnedArray<juce::AudioIODeviceType> deviceTypesToAdd;

            try
            {
                tempDeviceManager.createAudioDeviceTypes(deviceTypesToAdd);
                LOG("[AudioManager] Created " << deviceTypesToAdd.size() << " device types");
            }
            catch (const std::exception& e)
            {
                LOG_ERROR("[AudioManager] Exception creating device types: " << e.what());
                m_devicesCached = true;
                m_devicesCachedAtomic = true;
                return;
            }
            catch (...)
            {
                LOG_ERROR("[AudioManager] Unknown exception creating device types");
                m_devicesCached = true;
                m_devicesCachedAtomic = true;
                return;
            }

            for (int i = deviceTypesToAdd.size() - 1; i >= 0; --i)
            {
                if (m_shouldStopScan)
                {
                    LOG("[AudioManager] Device scan interrupted");
                    return;
                }

                auto* deviceType = deviceTypesToAdd[i];
                if (deviceType == nullptr)
                    continue;

                juce::String typeName = deviceType->getTypeName();

                try
                {
                    #if JUCE_ASIO
                    if (typeName == "ASIO" && !includeASIO)
                    {
                        LOG("[AudioManager] Skipping ASIO (not requested)");
                        continue;
                    }

                    if (typeName == "ASIO" && includeASIO)
                    {
                        LOG("[AudioManager] Scanning ASIO devices (this may take several seconds)...");
                        try
                        {
                            deviceType->scanForDevices();
                            juce::StringArray asioDevices = deviceType->getDeviceNames();

                            if (asioDevices.isEmpty())
                            {
                                LOG("[AudioManager] No ASIO devices found, skipping ASIO driver type");
                                continue;
                            }
                            else
                            {
                                LOG("[AudioManager] Found " << asioDevices.size() << " ASIO device(s)");
                            }
                        }
                        catch (const std::exception& e)
                        {
                            LOG_WARNING("[AudioManager] Exception scanning ASIO: " << e.what() << " - Skipping");
                            continue;
                        }
                        catch (...)
                        {
                            LOG_WARNING("[AudioManager] Unknown exception scanning ASIO - Skipping");
                            continue;
                        }
                    }
                    #endif

                    std::unique_ptr<juce::AudioIODeviceType> typePtr(
                        deviceTypesToAdd.removeAndReturn(i));
                    tempDeviceManager.addAudioDeviceType(std::move(typePtr));

                    LOG("[AudioManager] Added device type: " << typeName.toStdString());
                }
                catch (const std::exception& e)
                {
                    LOG_WARNING("[AudioManager] Exception adding device type '"
                               << typeName.toStdString() << "': " << e.what());
                }
                catch (...)
                {
                    LOG_WARNING("[AudioManager] Unknown exception adding device type: "
                               << typeName.toStdString());
                }
            }
            #endif

            const auto& deviceTypes = tempDeviceManager.getAvailableDeviceTypes();
            LOG("[AudioManager] Available device types: " << deviceTypes.size());

            for (int typeIndex = 0; typeIndex < deviceTypes.size(); ++typeIndex)
            {
                if (m_shouldStopScan)
                {
                    LOG("[AudioManager] Device scan interrupted");
                    return;
                }

                auto* deviceType = deviceTypes[typeIndex];
                if (deviceType == nullptr)
                    continue;

                juce::String typeName = deviceType->getTypeName();

                try
                {
                    LOG("[AudioManager] Scanning devices for type: " << typeName.toStdString());
                    deviceType->scanForDevices();
                    juce::StringArray deviceNames = deviceType->getDeviceNames();
                    LOG("[AudioManager] Found " << deviceNames.size()
                        << " device(s) for type: " << typeName.toStdString());

                    for (int i = 0; i < deviceNames.size(); ++i)
                    {
                        if (m_shouldStopScan)
                        {
                            LOG("[AudioManager] Device scan interrupted");
                            return;
                        }

                        juce::String deviceName = deviceNames[i];

                        DeviceInfo info;
                        info.name = deviceName.toStdString();
                        info.typeName = deviceType->getTypeName().toStdString();
                        info.maxInputChannels  = -1; // not yet queried (lazy loading)
                        info.maxOutputChannels = -1;

                        m_cachedDevices.push_back(info);
                        LOG("[AudioManager]   - " << info.name << " (" << info.typeName << ")");
                    }
                }
                catch (const std::exception& e)
                {
                    LOG_WARNING("[AudioManager] Exception scanning device type '"
                               << typeName.toStdString() << "': " << e.what());
                }
                catch (...)
                {
                    LOG_WARNING("[AudioManager] Unknown exception scanning device type: "
                               << typeName.toStdString());
                }
            }
        }
        catch (const std::exception& e)
        {
            LOG_ERROR("[AudioManager] Critical exception during device scan: " << e.what());
        }
        catch (...)
        {
            LOG_ERROR("[AudioManager] Critical unknown exception during device scan");
        }

        m_devicesCached = true;
        m_devicesCachedAtomic = true;

        auto endTime = std::chrono::high_resolution_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);

        LOG("[AudioManager] Device scan completed in " << duration.count() << " ms");
        LOG("[AudioManager] Total devices found: " << m_cachedDevices.size());
    }

    void AudioManager::scanDevicesAsync(bool includeASIO)
    {
        if (m_scanThread.joinable())
            m_scanThread.join();

        m_devicesCached = false;
        m_devicesCachedAtomic = false;
        m_shouldStopScan = false;

        m_scanThread = std::thread([this, includeASIO]() {
            scanAndCacheDevices(includeASIO);
        });
    }

    int AudioManager::getCachedDeviceCount() const
    {
        std::lock_guard<std::mutex> lock(m_deviceCacheMutex);
        return static_cast<int>(m_cachedDevices.size());
    }

    DeviceInfo AudioManager::getCachedDeviceInfo(int index) const
    {
        std::lock_guard<std::mutex> lock(m_deviceCacheMutex);

        if (index >= 0 && index < static_cast<int>(m_cachedDevices.size()))
            return m_cachedDevices[index];

        return DeviceInfo();
    }

    void AudioManager::refreshDevices()
    {
        scanAndCacheDevices(false);
    }

    bool AudioManager::isDeviceScanComplete() const
    {
        return m_devicesCachedAtomic.load();
    }

    bool AudioManager::waitForDeviceScan(int timeoutMs)
    {
        if (m_devicesCachedAtomic)
            return true;

        auto startTime = std::chrono::steady_clock::now();

        while (!m_devicesCachedAtomic)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));

            if (timeoutMs > 0)
            {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - startTime);

                if (elapsed.count() >= timeoutMs)
                {
                    LOG_WARNING("[AudioManager] Device scan timeout after " << elapsed.count() << " ms");
                    return false;
                }
            }
        }

        return true;
    }

    void AudioManager::refreshDevices(bool includeASIO, bool async)
    {
        if (async)
        {
            LOG("[AudioManager] Starting async device scan...");
            scanDevicesAsync(includeASIO);
        }
        else
        {
            LOG("[AudioManager] Starting synchronous device scan...");
            scanAndCacheDevices(includeASIO);
        }
    }

    DeviceInfo AudioManager::getDetailedDeviceInfo(int index)
    {
        std::lock_guard<std::mutex> lock(m_deviceCacheMutex);

        if (index < 0 || index >= static_cast<int>(m_cachedDevices.size()))
        {
            LOG_WARNING("[AudioManager] Invalid device index: " << index);
            return DeviceInfo();
        }

        DeviceInfo& info = m_cachedDevices[index];

        if (info.maxInputChannels != -1 && info.maxOutputChannels != -1)
            return info;

        if (info.maxOutputChannels == 0)
            return info;

        LOG("[AudioManager] Querying detailed info for device: " << info.name);

        try
        {
            juce::AudioDeviceManager tempDeviceManager;

            #if JUCE_WINDOWS
            juce::OwnedArray<juce::AudioIODeviceType> deviceTypesToAdd;
            tempDeviceManager.createAudioDeviceTypes(deviceTypesToAdd);

            for (int i = deviceTypesToAdd.size() - 1; i >= 0; --i)
            {
                std::unique_ptr<juce::AudioIODeviceType> typePtr(
                    deviceTypesToAdd.removeAndReturn(i));
                tempDeviceManager.addAudioDeviceType(std::move(typePtr));
            }
            #endif

            const auto& deviceTypes = tempDeviceManager.getAvailableDeviceTypes();

            for (auto* deviceType : deviceTypes)
            {
                if (deviceType->getTypeName().toStdString() == info.typeName)
                {
                    deviceType->scanForDevices();

                    std::unique_ptr<juce::AudioIODevice> device(
                        deviceType->createDevice(juce::String(info.name), juce::String(info.name)));

                    if (device != nullptr)
                    {
                        info.maxInputChannels  = device->getInputChannelNames().size();
                        info.maxOutputChannels = device->getOutputChannelNames().size();

                        LOG("[AudioManager] Device '" << info.name << "': "
                            << info.maxOutputChannels << " outputs, "
                            << info.maxInputChannels << " inputs");
                    }
                    else
                    {
                        info.maxInputChannels  = 0;
                        info.maxOutputChannels = 0;
                        LOG_WARNING("[AudioManager] Device '" << info.name
                            << "' is not available (not connected)");
                    }
                    break;
                }
            }
        }
        catch (const std::exception& e)
        {
            LOG_ERROR("[AudioManager] Exception querying device details: " << e.what());
            info.maxInputChannels  = 0;
            info.maxOutputChannels = 0;
        }
        catch (...)
        {
            LOG_ERROR("[AudioManager] Unknown exception querying device details");
            info.maxInputChannels  = 0;
            info.maxOutputChannels = 0;
        }

        return info;
    }

    void AudioManager::filterUnavailableDevices()
    {
        std::lock_guard<std::mutex> lock(m_deviceCacheMutex);

        LOG("[AudioManager] Filtering unavailable devices...");
        int validCount = 0;
        int invalidCount = 0;

        try
        {
            juce::AudioDeviceManager tempDeviceManager;

            #if JUCE_WINDOWS
            juce::OwnedArray<juce::AudioIODeviceType> deviceTypesToAdd;
            tempDeviceManager.createAudioDeviceTypes(deviceTypesToAdd);

            for (int i = deviceTypesToAdd.size() - 1; i >= 0; --i)
            {
                std::unique_ptr<juce::AudioIODeviceType> typePtr(
                    deviceTypesToAdd.removeAndReturn(i));
                tempDeviceManager.addAudioDeviceType(std::move(typePtr));
            }
            #endif

            const auto& deviceTypes = tempDeviceManager.getAvailableDeviceTypes();

            for (auto& info : m_cachedDevices)
            {
                if (info.maxOutputChannels != -1)
                    continue;

                bool deviceValid = false;

                for (auto* deviceType : deviceTypes)
                {
                    if (deviceType->getTypeName().toStdString() == info.typeName)
                    {
                        try
                        {
                            deviceType->scanForDevices();

                            std::unique_ptr<juce::AudioIODevice> device(
                                deviceType->createDevice(juce::String(info.name), juce::String(info.name)));

                            if (device != nullptr)
                            {
                                info.maxInputChannels  = device->getInputChannelNames().size();
                                info.maxOutputChannels = device->getOutputChannelNames().size();
                                deviceValid = true;
                                validCount++;

                                LOG("[AudioManager] Device '" << info.name << "': "
                                    << info.maxOutputChannels << " outputs, "
                                    << info.maxInputChannels << " inputs");
                            }
                        }
                        catch (const std::exception& e)
                        {
                            LOG_WARNING("[AudioManager] Exception validating device '"
                                << info.name << "': " << e.what());
                        }
                        catch (...)
                        {
                            LOG_WARNING("[AudioManager] Unknown exception validating device '"
                                << info.name << "'");
                        }

                        break;
                    }
                }

                if (!deviceValid)
                {
                    info.maxInputChannels  = 0;
                    info.maxOutputChannels = 0;
                    invalidCount++;
                    LOG_WARNING("[AudioManager] Device '" << info.name
                        << "' marked as unavailable (not connected or error)");
                }
            }
        }
        catch (const std::exception& e)
        {
            LOG_ERROR("[AudioManager] Exception during device filtering: " << e.what());
        }
        catch (...)
        {
            LOG_ERROR("[AudioManager] Unknown exception during device filtering");
        }

        LOG("[AudioManager] Device filtering complete: "
            << validCount << " valid, " << invalidCount << " unavailable");
    }

    // ========================================================================
    // AUDIO FILE MANAGEMENT
    // ========================================================================

    AudioFileMetadata AudioManager::getAudioFileMetadata(const std::string& filepath)
    {
        AudioFileMetadata metadata;

        juce::AudioFormatManager formatManager;
        formatManager.registerBasicFormats();

        juce::File audioFile(filepath);

        if (!audioFile.existsAsFile())
        {
            juce::Logger::writeToLog("AudioManager::getAudioFileMetadata() - File not found: " + juce::String(filepath));
            return metadata;
        }

        std::unique_ptr<juce::AudioFormatReader> reader(
            formatManager.createReaderFor(audioFile));

        if (reader == nullptr)
        {
            juce::Logger::writeToLog("AudioManager::getAudioFileMetadata() - Unable to read file: " + juce::String(filepath));
            return metadata;
        }

        metadata.numChannels  = static_cast<int>(reader->numChannels);
        metadata.sampleRate   = reader->sampleRate;
        metadata.totalSamples = reader->lengthInSamples;

        if (metadata.sampleRate > 0.0)
            metadata.lengthSeconds = static_cast<double>(metadata.totalSamples) / metadata.sampleRate;

        metadata.valid = true;

        return metadata;
    }

    // ========================================================================
    // PLAYER MANAGEMENT
    // ========================================================================

    void AudioManager::addPlayer(int* uid, bool is3D, bool isLooping)
    {
        m_spatializationEngine.addPlayer(uid, is3D, isLooping);
    }

    bool AudioManager::setPlayerFilePath(int uid, const char* path)
    {
        if (path == nullptr)
            return false;

        juce::String jucePath(path);
        return m_spatializationEngine.setPlayerFilePath(uid, jucePath);
    }

    bool AudioManager::removePlayer(int uid)
    {
        return m_spatializationEngine.removePlayer(uid);
    }

    // ========================================================================
    // AUDIO DEVICE SETUP
    // ========================================================================

    bool AudioManager::setup(const std::string& deviceName,
        int numInputChannels,
        int numVirtualSpeakers,
        int bufferSize,
        bool isBinauralVirtualization)
    {
        // ── Wait for the async device scan to finish ─────────────────────────────
        // AudioManager's constructor starts an asynchronous device scan.  On Windows
        // the scan includes ASIO drivers, which hold exclusive COM/driver resources
        // while running.  If setup() is called before the scan thread completes,
        // SpatializationEngine::setup() → m_deviceManager.initialise() may fail
        // because the background thread still owns those resources.
        // Waiting here (up to 10 s) is safe: the call is cheap when the scan has
        // already finished (atomic check), and it eliminates the race on every platform.
        if (!m_devicesCachedAtomic.load())
        {
            LOG("AudioManager::setup - waiting for device scan to complete...");
            waitForDeviceScan(10000);
        }

        // Store the binaural flag and virtual speaker count before configuring the engine
        m_isBinauralVirtualization = isBinauralVirtualization;
        m_numVirtualSpeakers = numVirtualSpeakers;

        // In binaural mode the output is always stereo regardless of the number of
        // virtual speakers.  In direct WFS mode numVirtualSpeakers equals the number
        // of physical output channels opened on the audio device.
        int physicalOutputChannels = isBinauralVirtualization ? 2 : numVirtualSpeakers;

        // ── Channel count guard (non-binaural mode only) ─────────────────────────
        // Requesting more physical output channels than the device supports causes
        // JUCE to crash inside AudioDeviceManager::initialise() with no recoverable
        // exception.  The check uses the cached device list populated by
        // filterUnavailableDevices(); it is skipped when maxOutputChannels == -1
        // (not yet queried) to avoid blocking setup on a missing device entry.
        if (!isBinauralVirtualization)
        {
            std::lock_guard<std::mutex> lock(m_deviceCacheMutex);
            for (const auto& dev : m_cachedDevices)
            {
                // Match by name, or accept the first entry when deviceName is empty
                // (default device selection).
                if (deviceName.empty() || dev.name == deviceName)
                {
                    if (dev.maxOutputChannels > 0 &&
                        physicalOutputChannels > dev.maxOutputChannels)
                    {
                        LOG_ERROR("AudioManager::setup - requested " << physicalOutputChannels
                            << " output channel(s) exceeds device maximum ("
                            << dev.maxOutputChannels << ") for device '"
                            << dev.name << "'. "
                            << "Reduce numVirtualSpeakers or enable binaural virtualization.");
                        return false;
                    }
                    break;
                }
            }
        }

        LOG("AudioManager::setup - Virtual speakers: " << numVirtualSpeakers
            << ", Physical outputs: " << physicalOutputChannels
            << ", Binaural: " << (isBinauralVirtualization ? "YES" : "NO"));

        try
        {
            m_spatializationEngine.setup(
                juce::String(deviceName),
                numInputChannels,
                physicalOutputChannels,
                bufferSize,
                numVirtualSpeakers,
                isBinauralVirtualization
            );
        }
        catch (const std::exception& e)
        {
            LOG_ERROR("AudioManager::setup - SpatializationEngine::setup() threw: " << e.what());
            return false;
        }
        catch (...)
        {
            LOG_ERROR("AudioManager::setup - SpatializationEngine::setup() threw an unknown exception.");
            return false;
        }

        return true;
    }

    void AudioManager::startPlayer(int uid)
    {
        m_spatializationEngine.startPlayers(uid);
    }

    void AudioManager::stopPlayer(int uid)
    {
        m_spatializationEngine.stopPlayers(uid);
    }

    void AudioManager::startAllPlayers()
    {
        m_spatializationEngine.startAllPlayers();
    }

    void AudioManager::stopAllPlayers()
    {
        m_spatializationEngine.stopAllPayers();
    }

    void AudioManager::stop()
    {
        m_isPlayerActive = false;
        m_spatializationEngine.stopAllPayers();
        m_spatializationEngine.close();
        m_spatializationEngine.releaseResources();
    }

    // ========================================================================
    // GLOBAL SPATIALIZATION ENGINE SETTINGS
    // ========================================================================

    void AudioManager::setListenerTransform(float* position, float* rotation, float* forward)
    {
        m_spatializationEngine.setListenerTransform(position, rotation, forward);
    }

    void AudioManager::setVirtualSpeakerTransform(float* positions, float* rotations, float* forwards, int virtualSpeakerCount)
    {
        m_spatializationEngine.setVirtualSpeakerTransform(positions, rotations, forwards, virtualSpeakerCount);
    }

    void AudioManager::setMaxDistanceForDelay(float maxDistance)
    {
        m_spatializationEngine.setMaxDistanceForDelay(maxDistance);
    }

    void AudioManager::setMasterGain(float masterGain)
    {
        m_spatializationEngine.setMasterGain(masterGain);
    }

    void AudioManager::setMakeupMasterGain(float makeupMasterGain)
    {
        m_spatializationEngine.setMakeupMasterGain(makeupMasterGain);
    }

    // ========================================================================
    // GLOBAL SPATIALIZATION ENGINE GETTERS
    // ========================================================================

    void AudioManager::getMixerOutputMeters(float* meters, int arraySize)
    {
        m_spatializationEngine.getMixerOutputMeters(meters, arraySize);
    }

    // ========================================================================
    // SPATIALIZATION ENGINE SETTINGS FOR PLAYERS
    // ========================================================================

    void AudioManager::setPlayerTransform(int uid, float* position, float* rotation, float* forward)
    {
        m_spatializationEngine.setPlayerTransform(uid, position, rotation, forward);
    }

    void AudioManager::setPlayerRealTimeParameter(int uid, float gain, float playbackSpeed, float attenuation, float minDistance)
    {
        m_spatializationEngine.setPlayerRealTimeParameter(uid, gain, playbackSpeed, attenuation, minDistance);
    }

    void AudioManager::enableAllPlayersSpeakerMask(bool isWfsSpeakerMask)
    {
        m_spatializationEngine.enableAllPlayersSpeakerMask(isWfsSpeakerMask);
    }

    void AudioManager::setIsWfsGain(bool isWfsGain)
    {
        m_spatializationEngine.setIsWfsGain(isWfsGain);
    }

    // ========================================================================
    // SPATIALIZATION ENGINE GETTERS FOR PLAYERS
    // ========================================================================

    void AudioManager::getPlayerWfsDelay(int uid, float* delay, int arraySize)
    {
        m_spatializationEngine.getPlayerWfsDelay(uid, delay, arraySize);
    }

    void AudioManager::getPlayerWfsLinGain(int uid, float* linGain, int arraySize)
    {
        m_spatializationEngine.getPlayerWfsLinGain(uid, linGain, arraySize);
    }

    void AudioManager::getPlayerMeters(int uid, float* meters, int arraySize)
    {
        m_spatializationEngine.getPlayerMeters(uid, meters, arraySize);
    }

    void AudioManager::getPlayerSpeakerMask(int uid, float* speakerMask, int arraySize)
    {
        m_spatializationEngine.getPlayerSpeakerMask(uid, speakerMask, arraySize);
    }

    void AudioManager::getPlayerNumChannel(int uid, int* numChannel)
    {
        m_spatializationEngine.getPlayerNumChannel(uid, numChannel);
    }

    void AudioManager::setIsPrefilterAllPlayers(bool isPrefilter)
    {
        m_spatializationEngine.setIsPrefilterAllPlayers(isPrefilter);
    }

    // ========================================================================
    // ENGINE ACCESS
    // ========================================================================

    SpatializationEngine* AudioManager::getSpatializationEngine()
    {
        return &m_spatializationEngine;
    }


    void AudioManager::setIsActiveSpeakersMinMax(bool enabled)
    {
        m_spatializationEngine.setIsActiveSpeakersMinMax(enabled);
    }

    void AudioManager::setSecondarySourceSize(float secondarySourceSize)
    {
        m_spatializationEngine.setSecondarySourceSize(secondarySourceSize);
    }

}
