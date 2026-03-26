/**
 * @file AT_AudioManager.h
 * @brief Main audio manager using JUCE framework
 * @author Antoine Gonot
 * @date 2026
 */

#pragma once

#include <JuceHeader.h>
#include "AT_AudioManagerListener.h"
#include "AT_SpatializationEngine.h"
#include <memory>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <mutex>

namespace AT
{
    /**
     * @struct DeviceInfo
     * @brief Information about an audio device
     */
    struct DeviceInfo
    {
        std::string name;           ///< Device name
        std::string typeName;       ///< Type (ASIO, CoreAudio, WASAPI, etc.)
        int maxInputChannels;       ///< Maximum number of input channels (-1 = unknown/not queried)
        int maxOutputChannels;      ///< Maximum number of output channels (-1 = unknown/not queried)

        DeviceInfo()
            : name("")
            , typeName("")
            , maxInputChannels(-1)
            , maxOutputChannels(-1)
        {}
    };

    /**
     * @struct AudioFileMetadata
     * @brief Metadata for an audio file
     */
    struct AudioFileMetadata
    {
        int numChannels;            ///< Number of channels
        double sampleRate;          ///< Sample rate in Hz
        double lengthSeconds;       ///< Duration in seconds
        long long totalSamples;     ///< Total number of samples
        bool valid;                 ///< True if data is valid

        AudioFileMetadata()
            : numChannels(0)
            , sampleRate(0.0)
            , lengthSeconds(0.0)
            , totalSamples(0)
            , valid(false)
        {}
    };

    /**
     * @class AudioManager
     * @brief Manages JUCE initialization, audio devices, and spatialization engine
     */
    class AudioManager : public AudioManagerListener
    {
    public:
        /**
         * @brief Constructor
         */
        AudioManager();

        /**
         * @brief Destructor
         */
        ~AudioManager();

        // ====================================================================
        // AudioManagerListener interface implementation
        // ====================================================================

        /**
         * @brief Prepares the audio system for playback
         * @param samplesPerBlock Number of samples per audio block
         * @param sampleRate Sample rate in Hz
         */
        void prepareToPlay(int samplesPerBlock, double sampleRate) override;

        /**
         * @brief Releases audio resources
         */
        void releaseResources() override;

        // ====================================================================
        // DEVICE ENUMERATION
        // ====================================================================

        /**
         * @brief Returns the number of cached devices
         * @return Number of devices
         */
        int getCachedDeviceCount() const;

        /**
         * @brief Returns cached device information
         * @param index Device index
         * @return Device information, or empty DeviceInfo if index is invalid
         */
        DeviceInfo getCachedDeviceInfo(int index) const;

        /**
         * @brief Refreshes the device cache (legacy synchronous method)
         */
        void refreshDevices();

        /**
         * @brief Check if device scan is complete
         * @return True if scan finished, false if still in progress
         */
        bool isDeviceScanComplete() const;

        /**
         * @brief Wait for device scan to complete (blocking)
         * @param timeoutMs Maximum time to wait in milliseconds (0 = infinite)
         * @return True if scan completed, false if timeout
         */
        bool waitForDeviceScan(int timeoutMs = 5000);

        /**
         * @brief Refresh devices with options
         * @param includeASIO If true, scan ASIO devices (slower)
         * @param async If true, scan asynchronously (non-blocking)
         */
        void refreshDevices(bool includeASIO, bool async);

        /**
         * @brief Get detailed info for a specific device (lazy loading)
         *
         * Creates the device temporarily to query channel counts.
         * Use sparingly — can be slow (1–2 seconds per device).
         *
         * @param index Device index
         * @return DeviceInfo with actual channel counts
         */
        DeviceInfo getDetailedDeviceInfo(int index);

        /**
         * @brief Validate all cached devices and mark unavailable ones.
         *
         * Devices that cannot be created are marked with maxOutputChannels = 0.
         * Should be called after the initial device scan completes.
         */
        void filterUnavailableDevices();

        // ====================================================================
        // AUDIO FILE MANAGEMENT
        // ====================================================================

        /**
         * @brief Retrieves metadata from an audio file without loading it
         *
         * Reads only the file headers to extract channel count, sample rate,
         * and duration. Much faster than loading the entire file.
         *
         * Supported formats: WAV, AIFF, FLAC, OGG, MP3
         *
         * @param filepath Path to the audio file
         * @return AudioFileMetadata structure with the file information
         *
         * @note Thread-safe; can be called from any thread
         *
         * @code
         * auto meta = audioManager.getAudioFileMetadata("/path/to/file.wav");
         * if (meta.valid)
         *     printf("Channels: %d, Rate: %.0f Hz\n", meta.numChannels, meta.sampleRate);
         * @endcode
         */
        AudioFileMetadata getAudioFileMetadata(const std::string& filepath);

        // ====================================================================
        // PLAYER MANAGEMENT
        // ====================================================================

        /**
         * @brief Adds a new player without loading a file
         * @param uid Pointer to receive the unique player ID
         * @param is3D True for 3D (spatialized) playback, false for 2D
         * @param isLooping True to loop the audio file
         */
        void addPlayer(int* uid, bool is3D, bool isLooping);

        /**
         * @brief Sets or changes the audio file for a player
         * @param uid Unique player identifier
         * @param path Full path to the audio file
         * @return True if the file was loaded successfully
         */
        bool setPlayerFilePath(int uid, const char* path);

        /**
         * @brief Removes a player
         * @param uid Unique player identifier
         * @return True if the player was removed successfully
         */
        bool removePlayer(int uid);

        // ====================================================================
        // AUDIO DEVICE SETUP
        // ====================================================================

        /**
         * @brief Configures the audio device and starts the engine
         *
         * @param deviceName Device name (empty = default)
         * @param numInputChannels Number of input channels
         * @param numVirtualSpeakers Number of virtual speakers used for WFS when
         *        binaural virtualization is enabled (physical output = 2 channels),
         *        or the number of physical output channels otherwise
         * @param bufferSize Buffer size in samples (0 = auto)
         * @param isBinauralVirtualization True to enable binaural virtualization
         */
        void setup(const std::string& deviceName,
            int numInputChannels,
            int numVirtualSpeakers,
            int bufferSize,
            bool isBinauralVirtualization);

        /**
         * @brief Starts playback for a player
         * @param uid Player ID
         */
        void startPlayer(int uid);

        /**
         * @brief Stops playback for a player
         * @param uid Player ID
         */
        void stopPlayer(int uid);

        /**
         * @brief Starts all players
         */
        void startAllPlayers();

        /**
         * @brief Stops all players
         */
        void stopAllPlayers();

        /**
         * @brief Stops the audio engine
         */
        void stop();

        // ====================================================================
        // GLOBAL SPATIALIZATION ENGINE SETTINGS
        // ====================================================================

        void setListenerTransform(float* position, float* rotation, float* forward);
        void setVirtualSpeakerTransform(float* positions, float* rotations, float* forwards, int virtualSpeakerCount);
        void setMaxDistanceForDelay(float maxDistance);
        void setMasterGain(float masterGain);
        void setMakeupMasterGain(float makeupMasterGain);

        // ====================================================================
        // GLOBAL SPATIALIZATION ENGINE GETTERS
        // ====================================================================

        void getMixerOutputMeters(float* meters, int arraySize);

        // ====================================================================
        // SPATIALIZATION ENGINE SETTINGS FOR PLAYERS
        // ====================================================================

        void setPlayerTransform(int uid, float* position, float* rotation, float* forward);
        void setPlayerRealTimeParameter(int uid, float gain, float playbackSpeed, float attenuation, float minDistance);

        /**
         * @brief Sets the flag enabling or disabling the speaker activation mask
         *        applied to the output channels of the WFS algorithm
         *
         * The mask depends on the position of the player and the listener in the scene.
         *
         * @param uid Player ID
         * @param isWfsSpeakerMask Flag to enable or disable the speaker mask for this player
         */
        void enableAllPlayersSpeakerMask(bool isWfsSpeakerMask);

        /**
         * @brief Enables or disables per-speaker WFS amplitude weighting (cos(phi)/sqrt(r)).
         * Propagated to all active players via the SpatializationEngine.
         * @param isWfsGain  true = WFS gain active, false = unity gain (delay-only)
         */
        void setIsWfsGain(bool isWfsGain);
        void setIsActiveSpeakersMinMax(bool enabled);

        /**
         * @brief Sets the effective radius of the WFS secondary sources (loudspeakers).
         *
         * Delegates to SpatializationEngine::setSecondarySourceSize(). The same value
         * is forwarded to the visual shader by At_MasterOutput (Unity side).
         *
         * @param secondarySourceSize  Effective loudspeaker radius in metres.
         *                             0 = ideal point sources (original behaviour).
         *                             Default: 0.3 m.
         */
        void setSecondarySourceSize(float secondarySourceSize);
        
        // ====================================================================
        // SPATIALIZATION ENGINE GETTERS FOR PLAYERS
        // ====================================================================

        void getPlayerWfsDelay(int uid, float* delay, int arraySize);
        void getPlayerWfsLinGain(int uid, float* linGain, int arraySize);
        void getPlayerMeters(int uid, float* meters, int arraySize);
        void getPlayerSpeakerMask(int uid, float* speakerMask, int arraySize);
        void getPlayerNumChannel(int uid, int* numChannel);
        void setIsPrefilterAllPlayers(bool isPrefilter);

        // ====================================================================
        // NEAR FIELD CORRECTION FOR BINAURAL VIRTUALIZATION
        // ====================================================================
    
        void setIsNearFieldCorrection(bool enabled)
        {
            m_spatializationEngine.setIsNearFieldCorrection(enabled);
        }

        /**
         * @brief Sets the HRTF reference distance. Call once at setup time.
         * rVirtual and azimuthDeg are computed internally from scene geometry.
         */
        void setNearFieldCorrectionRRef(float rRef = 1.0f,
                                        float headRadius = AT::NearFieldCorrection::DEFAULT_HEAD_RADIUS)
        {
            m_spatializationEngine.setNearFieldCorrectionRRef(rRef, headRadius);
        }
        
        // ====================================================================
        // ENGINE ACCESS
        // ====================================================================

        /**
         * @brief Returns the spatialization engine
         * @return Pointer to the engine
         */
        SpatializationEngine* getSpatializationEngine();

    private:
        /**
         * @brief Scans and caches all available audio devices
         * @param includeASIO If true, also scan ASIO devices (slower)
         */
        void scanAndCacheDevices(bool includeASIO = false);

        /**
         * @brief Async wrapper for device scanning
         * @param includeASIO If true, scan ASIO devices
         */
        void scanDevicesAsync(bool includeASIO);

        // ====================================================================
        // PRIVATE MEMBERS
        // ====================================================================

        /// JUCE GUI initialization scope guard
        const juce::ScopedJuceInitialiser_GUI m_juceInitialiser;

        /// Spatialization engine
        SpatializationEngine m_spatializationEngine;

        /// Device cache
        std::vector<DeviceInfo> m_cachedDevices;

        /// True once the device cache has been populated
        bool m_devicesCached;

        /// Current device sample rate
        double m_deviceSampleRate;

        /// True while players are active
        bool m_isPlayerActive;

        int  m_numVirtualSpeakers;
        bool m_isBinauralVirtualization;

        /// Mutex for thread-safe device cache access
        mutable std::mutex m_deviceCacheMutex;

        /// Atomic flag set to true when the device scan has completed
        std::atomic<bool> m_devicesCachedAtomic{false};

        /// Background thread used for async device scanning
        std::thread m_scanThread;

        /// Set to true to signal the scan thread to abort
        std::atomic<bool> m_shouldStopScan{false};

        // Disable copying
        AudioManager(const AudioManager&) = delete;
        AudioManager& operator=(const AudioManager&) = delete;
    };
}
