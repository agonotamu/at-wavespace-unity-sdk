/**
 * @file AT_SpatializationEngine.h
 * @brief Audio spatialization engine managing multiple audio players
 * @author Antoine Gonot
 * @date 2025
 */

#pragma once
#include <JuceHeader.h>
#include <atomic>

#include "AT_AudioManagerListener.h"
#include "AT_SpatPlayer.h"
#include "HRTFProcessor.h"
#include "AT_NearFieldCorrection.h"

namespace AT
{
    /**
     * @class SpatializationEngine
     * @brief Main audio engine managing multiple players and audio device routing
     *
     * When three or more players are active, they are processed in parallel
     * using an internal thread pool.
     */
    class SpatializationEngine : public juce::AudioSource
    {
    public:
        /**
         * @brief Constructs the spatialization engine and initializes audio device
         */
        SpatializationEngine();

        /**
         * @brief Destructor — properly closes audio device and cleans up resources
         */
        ~SpatializationEngine();

        /**
         * @brief Sets the listener for audio lifecycle callbacks
         */
        void setListener(AudioManagerListener* listener);

        /**
         * @brief Initializes the audio device with specific device and channel configuration
         *
         * @param deviceName Name of the audio device to use (empty for default)
         * @param numInputChannels Number of input channels
         * @param numOutputChannels Number of physical output channels (2 if binaural, N otherwise)
         * @param bufferSize Buffer size in samples (0 = auto)
         * @param numVirtualSpeakers Number of virtual speakers for WFS calculation (always N)
         * @param isBinauralVirtualization True to enable binaural virtualization
         */
        void setup(juce::String deviceName, int numInputChannels, int numOutputChannels, int bufferSize, int numVirtualSpeakers, bool isBinauralVirtualization);

        /**
         * @brief Closes the audio device and removes callbacks
         */
        void close();

        /**
         * @brief Prepares the engine for audio playback (AudioSource override)
         */
        void prepareToPlay(int samplesPerBlock, double sampleRate) override;

        /**
         * @brief Releases all audio resources (AudioSource override)
         */
        void releaseResources() override;

        /**
         * @brief Generates the next audio block by mixing all active players (AudioSource override)
         */
        void getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill) override;

        /**
         * @brief Adds a new audio player without loading a file
         */
        void addPlayer(int* uid, bool is3D, bool isLooping);

        /**
         * @brief Sets or changes the audio file path for a specific player
         */
        bool setPlayerFilePath(int uid, juce::String path);

        /**
         * @brief Removes a player by its unique ID
         */
        bool removePlayer(int uid);

        /**
         * @brief Starts playback for a specific player
         */
        void startPlayers(int uid);

        /**
         * @brief Stops playback for a specific player
         */
        void stopPlayers(int uid);

        /**
         * @brief Starts playback for all players
         */
        void startAllPlayers();

        /**
         * @brief Stops playback for all players
         */
        void stopAllPayers();

        // ============================================================================
        // GLOBAL SPATIALIZATION ENGINE SETTINGS
        // ============================================================================

        void setListenerTransform(float* position, float* rotation, float* forward);
        void setVirtualSpeakerTransform(float* positions, float* rotations, float* forwards, int virtualSpeakerCount);
        void setMaxDistanceForDelay(float maxDistance);
        void setMasterGain(float masterGain);
        void setMakeupMasterGain(float makeupMasterGain);

        // ============================================================================
        // GLOBAL SPATIALIZATION ENGINE GETTERS
        // ============================================================================

        void getMixerOutputMeters(float* meters, int arraySize);

        // ============================================================================
        // SPATIALIZATION ENGINE SETTINGS FOR PLAYERS
        // ============================================================================

        void setPlayerTransform(int uid, float* position, float* rotation, float* forward);
        void setPlayerRealTimeParameter(int uid, float gain, float playbackSpeed, float attenuation, float minDistance);

        /**
         * @brief Sets the flag enabling or disabling the speaker activation mask
         *        applied to the WFS output channels
         *
         * The mask depends on the positions of the player and the listener in the scene.
         *
         * @param uid Player ID
         * @param isWfsSpeakerMask Flag to set for the given player
         */
        void enableAllPlayersSpeakerMask(bool isWfsSpeakerMask);

        /**
         * @brief Enables or disables per-speaker WFS amplitude weighting for all players.
         *
         * When enabled, each speaker's gain is computed as cos(φ) / √r where φ is the
         * angle between the speaker outward normal and the source–speaker vector, and r
         * is the source–speaker distance. When disabled, all gains are held at 1.0
         * (delay-only WFS mode).
         *
         * Propagated immediately to every active Spatializer. Safe to call from the
         * main thread at any time.
         *
         * @param isWfsGain  true = apply WFS gain formula, false = unity gain (delay-only)
         */
        void setIsWfsGain(bool isWfsGain);
        void setIsActiveSpeakersMinMax(bool enabled);

        /**
         * @brief Sets the effective radius of the WFS secondary sources (loudspeakers).
         *
         * In WFS, secondary sources are the physical loudspeakers of the array.
         * Immediately forwarded to every active Spatializer and stored as the global
         * default so new players always start with the correct value.
         *
         * Controls P1 (amplitude regularisation: r_eff = sqrt(r²+ε²)) and
         * P2 (mask raised-cosine taper width ±ε) via a single shared parameter.
         *
         * @param secondarySourceSize  Effective loudspeaker radius in metres.
         *                             0 = ideal point sources (original behaviour).
         *                             Default: 0.3 m.
         */
        void setSecondarySourceSize(float secondarySourceSize);
        
        // ============================================================================
        // SPATIALIZATION ENGINE GETTERS FOR PLAYERS
        // ============================================================================

        void getPlayerWfsDelay(int uid, float* delay, int arraySize);
        void getPlayerWfsLinGain(int uid, float* linGain, int arraySize);
        void getPlayerMeters(int uid, float* meters, int arraySize);
        void getPlayerSpeakerMask(int uid, float* speakerMask, int arraySize);
        void getPlayerNumChannel(int uid, int* numChannel);
        void setIsPrefilterAllPlayers(bool isPrefilter);

        // ============================================================================
        // BINAURAL VIRTUALIZATION
        // ============================================================================

        /**
         * @brief Sets the binaural mode flag (configuration only, no allocation).
         *        Must be called BEFORE setup().
         */
        void setIsBinauralMode(bool enabled);

        /**
         * @brief Legacy setter — use setIsBinauralMode() + setup() instead.
         * @deprecated
         */
        void setIsBinauralVirtualization(bool isBinauralVirtualization);

        /**
         * @brief Loads HRTF data from a text file into all HRTF processors
         */
        bool loadHRTFFile(const std::string& filePath);

        /**
         * @brief Loads the default built-in HRTF (simple generic HRTF)
         */
        bool loadDefaultHRTF();

        // ============================================================================
        // SIMPLE BINAURAL MODE
        // ============================================================================

        /**
         * @brief Sets simple binaural spatialization mode for A/B comparison
         */
        bool setIsSimpleBinauralSpat(bool isSimple);

        // ====================================================================
        // NEAR FIELD CORRECTION FOR BINAURAL VIRTUALIZATION
        // ====================================================================
        
        /**
         * @brief Enables or disables the near-field ILD correction on binaural output.
         *
         * When enabled, applies a first-order stereo shelving correction derived from the
         * rigid sphere model (Duda & Martens 1998) to the accumulated binaural buffer.
         * The correction boosts the low-frequency ILD for sources closer than the BRIR
         * reference distance, restoring the near-field perceptual cue absent from
         * distance-independent HRTF/BRIR sets.
         *
         * Only active in WFS binaural virtualization mode (m_isBinauralVirtualization=true).
         * No effect in simple binaural mode or WFS-only mode.
         *
         * @param enabled  true = correction applied, false = bypass.
         */
        void setIsNearFieldCorrection(bool enabled);

        /**
         * @brief Truncates HRTF IRs to 512 samples when enabled (0 = no limit).
         * Propagated to all HRTFProcessor instances immediately; takes effect on
         * the next IR reload (triggered by the next position update).
         * Reduces CPU load at high virtual-speaker counts at the cost of minor
         * low-frequency accuracy.
         */
        void setHrtfTruncate(bool enabled);

        /**
         * @brief Sets the HRTF reference distance for the near-field ILD correction.
         *
         * This is the only parameter that cannot be inferred from scene geometry —
         * it is a property of the HRTF/BRIR dataset (the distance at which the
         * impulse responses were measured). Call this once at setup time, or whenever
         * the HRTF set changes.
         *
         * rVirtual and azimuthDeg are recomputed internally on every
         * setListenerTransform() and setPlayerTransform() call, so the filter
         * always reflects current scene geometry without any explicit update
         * from the caller.
         *
         * @param rRef        Distance at which the BRIR/HRTF was measured (m).
         *                    Typically 1.0 m for a close-field HRTF set (default).
         * @param headRadius  Sphere/head radius (m). Default: KEMAR 0.0875 m.
         */
        void setNearFieldCorrectionRRef(float rRef       = 1.0f,
                                        float headRadius = AT::NearFieldCorrection::DEFAULT_HEAD_RADIUS);
        
        // ============================================================================
        // SMOOTHED GLOBAL PARAMETER ACCESSORS (used by Spatializer)
        // ============================================================================

        void getSmoothedListenerPosition(float& outX, float& outY, float& outZ) const;
        void getSmoothedListenerForward(float& outX, float& outY, float& outZ) const;
        void getSmoothedVirtualSpeakerPosition(int speakerIndex, float& outX, float& outY, float& outZ) const;
        float getSmoothedAzimuthForSpeaker(int speakerIndex) const;

        // ============================================================================
        // MULTITHREADING CONTROL
        // ============================================================================

        void setMultithreadingEnabled(bool enabled);
        bool isMultithreadingEnabled() const;

        /**
         * @brief Advance all global smoothers by one sample
         */
        void advanceGlobalSmoothers();

        /**
         * @brief Maximum number of virtual speakers supported.
         *
         * Must match Spatializer::MAX_VIRTUAL_SPEAKERS.
         * All per-speaker arrays in SpatializationEngine are statically allocated
         * to this size to eliminate heap fragmentation at PlayMode startup.
         *
         * Static RAM cost (SpatializationEngine singleton):
         *   - float[1024*3]                  =  12 KB  (speaker positions flat)
         *   - 4 × LinearSmoothedValue[1024]  = 112 KB  (position/azimuth smoothers)
         *   - 4 × float[1024]                =  16 KB  (smoothed values)
         *   - float[1024]                    =   4 KB  (meters)
         *   Total ≈ 144 KB
         */
        static constexpr int MAX_VIRTUAL_SPEAKERS = 1024;

        /**
         * @brief Maximum audio block size (samples) accepted by the engine.
         *
         * This is the SINGLE authoritative constant for buffer-size constraints.
         * It governs two things:
         *   1. The upper clamp applied to the requested bufferSize in setup()
         *      so the audio driver is never asked for more samples than we support.
         *   2. The allocation size of m_transitionGainBuffer, which is sized to
         *      the ACTUAL samplesPerBlock in prepareToPlay() and is therefore
         *      immune to driver rounding — but setup() still rejects any request
         *      above this limit to prevent unbounded memory growth.
         *
         * To raise the limit (e.g. to 8192 or 16384 for deferred-time playback),
         * change ONLY this constant. No other value in the codebase needs editing.
         *
         * Default: 4096 samples (~85 ms @ 48 kHz).
         */
        static constexpr int MAX_SAMPLES_PER_BLOCK = 8192;

    private:

        int m_numOutputChannels;
        int m_samplesPerBlock;
        double m_sampleRate;
        float m_masterGain       = 0;
        float m_makeupMasterGain = 0;

        /// Per-output-channel RMS meters (static array, indexed 0..m_numOutputChannels-1)
        float m_metersArray[MAX_VIRTUAL_SPEAKERS];

        /// Collection of managed audio players
        std::vector<std::unique_ptr<AT::SpatPlayer>> m_spatPlayers;

        AudioManagerListener* m_pListener = nullptr;

        std::unique_ptr<juce::AudioSourcePlayer> m_puPlayer;

        juce::AudioDeviceManager m_deviceManager;

        int m_lastID = -1;

        // ====================================================================
        // PRIVATE PROCESSING METHODS
        // ====================================================================

        void processPlayersWFS(const juce::AudioSourceChannelInfo& bufferToFill);
        void processBinauralVirtualization(const juce::AudioSourceChannelInfo& bufferToFill);
        void processSimpleBinaural(const juce::AudioSourceChannelInfo& bufferToFill);

        // ============================================================================
        // HRTF PROCESSORS FOR BINAURAL RENDERING
        // ============================================================================

        bool m_isBinauralVirtualization;

        /// HRTF processors for binaural rendering. Populated only when m_isBinauralVirtualization is true.
        std::vector<std::unique_ptr<HRTFProcessor>> m_puHrtfProcessors;

        /// Temporary stereo accumulation buffer for binaural rendering
        juce::AudioBuffer<float> m_binauralTemp;

        // Listener transform (for azimuth calculation)
        float m_listenerPosX    = 0.0f;
        float m_listenerPosY    = 0.0f;
        float m_listenerPosZ    = 0.0f;
        float m_listenerForwardX = 0.0f;
        float m_listenerForwardY = 0.0f;
        float m_listenerForwardZ = 1.0f;

        /// Background thread for async HRTF processor destruction.
        /// Joined in close() before audio device teardown.
        std::thread m_hrtfCleanupThread;
        
        // ====================================================================
        // NEAR FIELD CORRECTION FOR BINAURAL VIRTUALIZATION
        // ====================================================================
        
        /// Stereo near-field ILD correction filter
        AT::NearFieldCorrection m_nearFieldCorrection;

        /// True while near-field correction is active.
        std::atomic<bool> m_isNearFieldCorrection{false};

        /// HRTF/BRIR reference measurement distance (m) — set externally once.
        float m_nfcRRef       = 1.0f;
        float m_nfcHeadRadius = AT::NearFieldCorrection::DEFAULT_HEAD_RADIUS;

        /// Position of the NFC virtual source — updated on every setPlayerTransform().
        /// For the single-player corpus use case this is always correct.
        /// For multi-player scenes it tracks the most recently moved source.
        float m_nfcSourcePosX = 0.0f;
        float m_nfcSourcePosY = 0.0f;
        float m_nfcSourcePosZ = 1.0f;  ///< default: 1 m in front of listener

        /**
         * @brief Recomputes rVirtual and azimuthDeg from current listener/source
         *        positions and updates the NearFieldCorrection filter coefficients.
         * Called automatically by setListenerTransform() and setPlayerTransform().
         */
        void updateNearFieldCorrectionGeometry();
        
        // ============================================================================
        // VIRTUAL SPEAKER DATA  (statically pre-allocated)
        // ============================================================================

        /// Flat array of virtual speaker positions: [x0,y0,z0, x1,y1,z1, ...]
        float m_virtualSpeakerPositionsFlat[MAX_VIRTUAL_SPEAKERS * 3];

        int m_numVirtualSpeakers;

        // ============================================================================
        // FIX B — Thread-safe position staging (LinearSmoothedValue race condition)
        // ============================================================================
        // juce::LinearSmoothedValue is NOT thread-safe. setTargetValue() (main thread,
        // via setListenerTransform/setVirtualSpeakerTransform) races with getNextValue()
        // (audio thread, via advanceGlobalSmoothers). This causes random corruption of
        // the ramp state → audible glitches even during slow movement.
        //
        // Fix: main thread writes ONLY into staging buffers under m_positionLock.
        // Audio thread adopts them once per block (step 0) and calls setTargetValue()
        // from the audio thread only — serialised with getNextValue().
        // ============================================================================
        mutable juce::SpinLock m_positionLock;

        float             m_pendingListenerPosition[3] = {};
        float             m_pendingListenerRotation[3] = {};
        float             m_pendingListenerForward[3]  = {};
        std::atomic<bool> m_listenerTransformDirty{false};

        float             m_pendingSpeakerPositions[MAX_VIRTUAL_SPEAKERS * 3] = {};
        float             m_pendingSpeakerRotations[MAX_VIRTUAL_SPEAKERS * 3] = {};
        float             m_pendingSpeakerForwards [MAX_VIRTUAL_SPEAKERS * 3] = {};
        int               m_pendingSpeakerCount = 0;
        std::atomic<bool> m_speakerTransformDirty{false};

        // Pre-allocated adopt buffers (avoid ~36 KB stack allocation on audio thread)
        float m_adoptPosBuf[MAX_VIRTUAL_SPEAKERS * 3];
        float m_adoptRotBuf[MAX_VIRTUAL_SPEAKERS * 3];
        float m_adoptFwdBuf[MAX_VIRTUAL_SPEAKERS * 3];

        /// N-channel WFS output buffer (only used in binaural mode).
        /// setSize() is called once in prepareToPlay() — no per-block allocation.
        juce::AudioBuffer<float> m_wfsBuffer;

        /**
         * @brief Per-channel hold counter for the HRTF convolution bypass.
         *
         * When a WFS channel's peak falls below HRTF_SKIP_THRESHOLD, its counter
         * is incremented each block. The convolution is only skipped once the counter
         * exceeds HRTF_HOLD_BLOCKS, giving the convolver tail time to drain silently.
         * Resets to 0 as soon as the channel becomes active again.
         * Prevents the noise-gate artefact heard when a low-level signal repeatedly
         * crosses the threshold.
         */
        int m_hrtfChannelHold[MAX_VIRTUAL_SPEAKERS] = {};
        
        /// Stereo accumulation buffers for parallel HRTF convolution — one per thread.
        /// Sized in prepareToPlay(), never reallocated in audio thread.
        std::vector<juce::AudioBuffer<float>> m_hrtfThreadBuffers;

        
        float calculateTargetAzimuthForSpeaker(int speakerIndex);

        /// Minimum number of virtual speakers required to activate HRTF multithreading
        static constexpr int MIN_SPEAKERS_FOR_HRTF_THREADING = 8;

        std::atomic<int> m_hrtfProcessed{0};

        // ============================================================================
        // SIMPLE BINAURAL MODE
        // ============================================================================

        /// True when simple binaural mode is active
        bool m_isSimpleBinauralSpat = false;

        /// One dedicated HRTFProcessor per player (simple binaural mode)
        std::vector<std::unique_ptr<HRTFProcessor>> m_puSimpleBinauralPlayerProcessors;

        // ============================================================================
        // MODE TRANSITION CROSSFADE
        // ============================================================================

        juce::LinearSmoothedValue<float> m_transitionGain;   ///< 1.0 = full volume, 0.0 = silence
        bool m_isFadingToMode      = false;                   ///< True while fading out, mode switch pending
        bool m_targetModeAfterFade = false;                   ///< Target mode to apply when gain reaches 0

        /**
         * @brief Per-sample gain ramp buffer for the binaural↔WFS mode crossfade.
         *
         * Allocated in prepareToPlay() to the ACTUAL samplesPerBlock delivered by
         * the driver — NOT to MAX_SAMPLES_PER_BLOCK. This makes the buffer immune
         * to driver rounding (e.g. a request for 4096 rounded up to 8192 by ASIO):
         * prepareToPlay() always receives the true block size and allocates exactly
         * that many floats, so getNextAudioBlock() can never write out of bounds.
         *
         * Previously declared as float[MAX_SAMPLES_PER_BLOCK]. That caused a crash
         * whenever setup() was called a second time with a different buffer size,
         * because prepareToPlay() was not resizing the array.
         */
        std::unique_ptr<float[]> m_transitionGainBuffer;

        bool m_hasPendingModeChangeDuringWarmup = false;
        bool m_pendingModeAfterWarmup = false;

        // ============================================================================
        // SMOOTHED PARAMETER MANAGEMENT  (statically pre-allocated)
        // ============================================================================

        static constexpr float SMOOTHING_TIME_SECONDS          = 0.05f;
        static constexpr float GLOBAL_SMOOTHING_TIME_SECONDS   = 0.05f;
        static constexpr float BINAURALMODE_SMOOTHING_TIME_SECONDS = 0.05f;
        static constexpr int   HRTF_POSITION_UPDATE_CHUNK_SIZE = 64;
        int m_warmupBlocksRemaining = 0;
        static constexpr int WARMUP_BLOCKS = 8; ///< ~8 blocks @ 512 samples ≈ 85 ms at 48 kHz

        void updateGlobalParametersTarget();
        void initializeGlobalSmoothers();

        // Listener smoothers (scalar)
        juce::LinearSmoothedValue<float> m_listenerPosXSmoother;
        juce::LinearSmoothedValue<float> m_listenerPosZSmoother;
        juce::LinearSmoothedValue<float> m_listenerPosYSmoother;
        juce::LinearSmoothedValue<float> m_listenerForwardXSmoother;
        juce::LinearSmoothedValue<float> m_listenerForwardYSmoother;
        juce::LinearSmoothedValue<float> m_listenerForwardZSmoother;

        /// Per-speaker position smoothers (static arrays)
        juce::LinearSmoothedValue<float> m_virtualSpeakerPosXSmoother[MAX_VIRTUAL_SPEAKERS];
        juce::LinearSmoothedValue<float> m_virtualSpeakerPosYSmoother[MAX_VIRTUAL_SPEAKERS];
        juce::LinearSmoothedValue<float> m_virtualSpeakerPosZSmoother[MAX_VIRTUAL_SPEAKERS];
        juce::LinearSmoothedValue<float> m_virtualSpeakerAzimuthSmoother[MAX_VIRTUAL_SPEAKERS];

        // Current smoothed listener values (scalars)
        float m_smoothedListenerPosX;
        float m_smoothedListenerPosY;
        float m_smoothedListenerPosZ;
        float m_smoothedListenerForwardX;
        float m_smoothedListenerForwardY;
        float m_smoothedListenerForwardZ;

        /// Current smoothed speaker values (static arrays)
        float m_smoothedSpeakerPosX[MAX_VIRTUAL_SPEAKERS];
        float m_smoothedSpeakerPosY[MAX_VIRTUAL_SPEAKERS];
        float m_smoothedSpeakerPosZ[MAX_VIRTUAL_SPEAKERS];
        float m_smoothedSpeakerAzimuth[MAX_VIRTUAL_SPEAKERS];

        // ============================================================================
        // MULTITHREADING
        // ============================================================================

        juce::ThreadPool m_audioThreadPool;
        std::atomic<int> m_playersProcessed{0};
        std::vector<std::unique_ptr<juce::AudioBuffer<float>>> m_playerBuffers;
        std::atomic<bool> m_useMultithreading{false};
        static constexpr int MIN_PLAYERS_FOR_THREADING = 3;

        // ============================================================================
        // THREAD-SAFE MODE SWITCHING
        // ============================================================================

        std::atomic<bool> m_pendingModeChange{false};
        std::atomic<bool> m_targetSimpleBinauralMode{false};

        // ============================================================================
        // THREAD-SAFETY — player list
        // ============================================================================

        /**
         * @brief SpinLock protecting m_spatPlayers and m_playersGraveyard.
         *
         * Held only for microseconds (pointer copies or vector erase).
         * Safe to acquire from both the main thread and the audio thread.
         */
        mutable juce::SpinLock m_playerListLock;

        /**
         * @brief Snapshot of raw player pointers, rebuilt once per audio block.
         *
         * The audio thread iterates this snapshot exclusively — it is immune to
         * concurrent add/remove operations on m_spatPlayers from the main thread.
         * Only ever written/read from the single audio callback thread; no lock
         * needed after the snapshot has been populated at the start of
         * getNextAudioBlock().
         */
        std::vector<AT::SpatPlayer*> m_playerSnapshot;

        /**
         * @brief Players removed from m_spatPlayers but not yet destroyed.
         *
         * removePlayer() (main thread) moves the unique_ptr here instead of
         * calling the destructor immediately. The audio thread drains this vector
         * at the very start of the next getNextAudioBlock() call, once it is
         * guaranteed that no snapshot from the previous block still holds a raw
         * pointer to these players. Destruction happens outside the SpinLock to
         * avoid blocking the audio thread on potentially slow destructors.
         */
        std::vector<std::unique_ptr<AT::SpatPlayer>> m_playersGraveyard;
        
        
        // ── Cached global player flags ──────────────────────────────────────────
        // Stored so that players added AFTER the global setter is called also
        // receive the correct initial flag value.
        bool m_globalIsWfsSpeakerMask    = true;
        bool  m_globalIsPrefilter            = false;
        bool  m_globalIsWfsGain              = false;
        bool  m_globalIsActiveSpeakersMinMax = false;
        float m_globalSecondarySourceSize             = 0.3f;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SpatializationEngine)
    };
}
