/**
 * @file AT_Spatializer.h
 * @brief To do
 * @author Antoine Gonot
 * @date 2025
 */

#pragma once

#include <JuceHeader.h>
#include <memory>
#include <atomic>

namespace AT
{
    // Forward declarations
    class SpatPlayer;
    class SpatializationEngine;
    
    /**
     * @class Spatializer
     * @brief To do
     *
     * To do
     */
    class Spatializer
    {
    public:
        
        // ============================================================================
        // STATIC CONSTANT : maximum number of virtual speakers / output channels
        // ============================================================================
        
        /**
         * @brief Maximum number of virtual speakers (output channels) supported.
         *
         * All arrays that depend on the speaker count are statically allocated to this
         * size, eliminating thousands of heap allocations at PlayMode startup and
         * greatly reducing initialization time when N is large (e.g. 1024 for WFS).
         *
         * RAM cost per Spatializer instance (static arrays only):
         *   - 3 × float[1024][3]  = 36 KB  (speaker positions/rotations/forwards)
         *   - 4 × float[1024]     = 16 KB  (gains, delays, masks, work buffer)
         *   - LinearSmoothedValue[1024] ≈ 28 KB
         *   Total ≈ 80 KB per Spatializer
         */
        static constexpr int MAX_VIRTUAL_SPEAKERS = 1024;

        /**
         * @brief Constructs a new Spatializer instance
         * 
         * @param spatPlayer Reference to the SpatPlayer instance that owns this spatializer
         * @param numOutputChannel Number of output channels
         * @param samplesPerBlock Number of samples per processing block
         * @param sampleRate Sample rate in Hz
         */
        Spatializer(SpatPlayer& spatPlayer, int numOutputChannel, int samplesPerBlock, double sampleRate);
        
        /**
         * @brief Destructor of the Spatializer instance
         *
         * To do
         */
        ~Spatializer();

        float spatialize(float inputSample, int indexChannel, bool updateReadPointer = true);
                
        // ============================================================================
        // GLOBAL SPATIALIZATION ENGINE SETTINGS
        // ============================================================================

        /**
         * @brief set the position, rotation and forward vectors of the listener is the scene.
         */
        void setListenerTransform(float* position, float* rotation, float* forward);

        /**
         * @brief set the position and orientation of the virtual speaker
         */
        void setVirtualSpeakerTransform(float* positions, float* rotations, float* forwards, int virtualSpeakerCount);
        
        /**
         * @brief set the maximum distance for a source from a virtual speaker used to setup the size of the delay line
         */
        void setMaxDistanceForDelay(float maxDistance);

        // ============================================================================
        // SPATIALIZATION ENGINE SETTINGS FOR PLAYERS
        // ============================================================================

        /**
         * @brief set the position, rotation and forward vectors of the player is the scene.
         */
        void setPlayerTransform(float* position, float* rotation, float* forward);

        /**
         * @brief set the power factor for player's attenuation as a function of distance to the listener.
         */
        void setPlayerAttenuation(float attenuation);

        /**
         * @brief set the minimum distance for the player used to calculate attenuation as a function of distance to the listener.
         */
        void setPlayerMinDistance(float minDistance);

        /**
         * @brief Sets the flag enabling or disabling the speaker activation mask  applied to the output channels of the WFS algorithm
         *
         * The mask depend on the position of the player and the listener in the scene
         *
         * @param isWfsSpeakerMask the flag to set for a given player
         */
        void enablePlayerSpeakerMask(bool isWfsSpeakerMask);
        
        /** Sets the WFS gain flag and propagates the target values to all smoothers immediately.
         *  Safe to call from the main thread at any time. */
        void setIsWfsGain(bool isWfsGain);

        /**
         * @brief When enabled, the time-reversal min/max reference is computed only
         * over active speakers (mask > 0.5). Prevents L/R inversion on focused sources
         * when the speaker mask is active. When disabled, all speakers are included
         * (original behaviour).
         */
        void setIsActiveSpeakersMinMax(bool enabled) { m_isActiveSpeakersMinMax.store(enabled); }

        /**
         * @brief Sets the effective radius (metres) of the WFS secondary sources (loudspeakers).
         *
         * In WFS theory, secondary sources are the physical loudspeakers of the array.
         * Modelling them as finite-size sources (radius ε) rather than ideal point sources
         * regularises two singularities that occur when the primary source (At_Player)
         * approaches or crosses the array plane:
         *
         *   P1 — Amplitude (udpateWfsGainAndDelay):
         *     r_eff = sqrt(r_primaryToSpeaker² + ε²) replaces r_raw in cos(φ)/sqrt(r).
         *     Prevents divergence when a primary source reaches the plane of the array.
         *     The delay path continues to use r_raw to preserve wavefront phase.
         *
         *   P2 — Mask taper (setIsInsideAndUpdateSpeakerMask):
         *     The hard gate (dotSource < 0 → mask = 0) is replaced by a C¹
         *     raised-cosine ramp over ±ε, eliminating the silence step at
         *     the non-focused ↔ focused source transition.
         *
         * Thread-safe: written from main thread, read from audio thread via atomic.
         * Same value is forwarded to the visual shader (_secondarySourceSize uniform).
         *
         * @param secondarySourceSize  Effective loudspeaker radius in metres.
         *                             0 = ideal point sources (original behaviour).
         *                             Typical: 0.05–0.5 m. Default: 0.3 m.
         */
        void setSecondarySourceSize(float secondarySourceSize)
        {
            m_secondarySourceSize.store(juce::jmax(0.0f, secondarySourceSize), std::memory_order_relaxed);
        }

        /** Must be called from prepareToPlay() to initialise the gain smoothers
         *  with the correct sample rate and ramp duration. */
        void prepareWfsGainSmoothers(double sampleRate, int samplesPerBlock);
        
        // ============================================================================
        // SPATIALIZATION ENGINE GETTERS FOR PLAYERS
        // ============================================================================

        /**
         * @brief get the delay in seconds apply in the driving function for each output channel
         */
        void getPlayerWfsDelay(float* delay, int arraySize);

        /**
         * @brief get the amplitude factor apply in the driving function for each output channel
         */
        void getWfsLinGain(float* linGain, int arraySize);

        /**
         * @brief set an activation flag for each output channel of the WFS spatialization algorithm
         */
        void getPlayerSpeakerMask(float* speakerMask, int arraySize);
                    
        /**
         * @brief Get source position
         */
        void getSourcePosition(float& x, float& y, float& z) const;
        
        /**
         * @brief Get listener position
         */
        void getListenerPosition(float& x, float& y, float& z) const;
        
        /**
         * @brief Get listener forward vector
         */
        void getListenerForward(float& x, float& y, float& z) const;
        
        /**
         * @brief Get the number of active channel in the speaker mask
         */
        float getNumActiveSpeakerInMask();

        /**
         * @brief Computes the distance attenuation gain for the current source position.
         *
         * Uses the listener position from the SpatializationEngine (smoothed) and
         * the raw source position set via setPlayerTransform().
         *
         * Formula:  gain = 1 / max(d, m_minDistance) ^ m_attenuation
         *
         * where d is the 3D Euclidean distance from source to listener.
         *
         * - When d ≤ m_minDistance: gain = 1.0 (no attenuation in the near-field).
         * - When m_attenuation = 0: gain = 1.0 always (no distance rolloff).
         * - The result is clamped to [0, 1] to prevent any boost.
         *
         * Intended to be called ONCE per audio block from SpatPlayer::processAndAdd()
         * and applied as a scalar multiplier on the block output, for both the WFS
         * and Simple Binaural paths.
         *
         * @return Linear gain in [0, 1].
         */
        float computeDistanceGain() const;
        
        /**
         * @brief Get the parent SpatializationEngine
         */
        SpatializationEngine* getSpatializationEngine() const;
        
        /**
         * @brief update the target value for each parameters used by the spatialization algorithm (call once per block)
         */
        void updateSourceParametersTarget();

        /**
         * @brief update smoothed value for each parameters used by the spatialization algorithm (call for each output sample)
         */
        void advanceSourceSmoothers();
        
        /**
         * @brief Set the parent SpatializationEngine (needed for accessing smoothed global parameters)
         */
        void setSpatializationEngine(SpatializationEngine* engine);
        
        /**
         * @brief delay line storing the history of the audio data
         */
        juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Linear> m_wfsDelayLine;

        /**
         * @brief Update the values in the speaker mask array
         */
        void setIsInsideAndUpdateSpeakerMask();
        
        /**
         * @brief Resets the WFS delay line to zero
         * Called when switching mode (Simple Binaural ↔ WFS) to avoid
         * stale samples causing transients on mode resume.
         */
        void resetDelayLine() { m_wfsDelayLine.reset(); }
        
        
    private:
        
        // ============================================================================
        // PRIVATE METHODS
        // ============================================================================
        
        /**
         * @brief Initialize all the arrays used in this class
         */
        void intializeArray(int numOutputChannel);
        
        /**
         * @brief Initialize all the dsp::DelayLine components
         */
        void intializeDelayLine(int sampleRate, int samplePerBlock);

        /**
         * @brief Core calculation of the driving function for WFS
         */
        void udpateWfsGainAndDelay();

        
        // ============================================================================
        // PRIVATE VARIABLES
        // ============================================================================

        /**
         * @brief Smoothing time in seconds for position and rotation parameters
         */
        static constexpr float SMOOTHING_TIME_SECONDS = 0.05f;
        
        bool m_isInside;

        /**
         * @brief Continuous blend factor for the focused-source time-reversal transition.
         *
         * Derived from m_workBufferOutsideMask at the end of setIsInsideAndUpdateSpeakerMask():
         *   m_insideBlend = mean over all speakers of (1 − mask_tapered[i])
         *
         *   0.0 = source fully outside  → normal delays
         *   1.0 = source fully inside   → fully time-reversed delays
         *   0…1 = source within ±secondarySourceSize of array plane → linear blend
         *
         * Used in udpateWfsGainAndDelay() to replace the hard inversion switch.
         */
        float m_insideBlend = 0.0f;

        /**
         * @brief Smoothed version of m_insideBlend.
         *
         * m_insideBlend is the raw per-block estimate (0 = outside, 1 = inside).
         * m_insideBlendSmoother interpolates it sample-by-sample so the delay
         * blend changes gradually, eliminating clicks at the array-plane crossing.
         */
        juce::LinearSmoothedValue<float> m_insideBlendSmoother;

        /**
         * @brief Smoothers for the time-reversal delay reference values.
         *
         * m_wfsMaxDelay / m_wfsMinDelay are recomputed each sample from a
         * threshold test on the speaker mask. In the transition zone, different
         * speakers cross the threshold on successive samples, making the reference
         * jump discontinuously → audible clicks in the blended delay.
         * Smoothing the reference eliminates those clicks.
         */
        juce::LinearSmoothedValue<float> m_wfsMaxDelaySmoother;
        juce::LinearSmoothedValue<float> m_wfsMinDelaySmoother;
                
        /**
         * @brief Reference to the SpatPlayer instance that owns this spatializer
         */
        SpatPlayer& m_spatPlayer;
        
        /**
         * @brief Pointer to the parent SpatializationEngine
         */
        SpatializationEngine* m_pSpatializationEngine;
        
        int m_numOutputChannels;
        int m_samplesPerBlock;
        double m_sampleRate;
        float m_maxDistance;

        float m_pSourcePosition[3];
        float m_pSourceRotation[3];
        float m_pSourceForward[3];
        float m_pListenerPosition[3];
        float m_pListenerRotation[3];
        float m_pListenerForward[3];
        
        // ============================================================================
        // PRE-ALLOCATED STATIC ARRAYS  (replaces unique_ptr<unique_ptr<float[]>[]>)
        // Allocated once at construction — zero heap fragmentation
        // ============================================================================

        
        // ============================================================================
        // DOUBLE BUFFER — output arrays
        //
        // m_wfsDelays / m_wfsLinGains are written by the audio thread each block.
        // The Unity main thread reads them concurrently for shader visualization.
        // To avoid partial snapshots, the audio thread memcpy's into committed
        // copies at the end of udpateWfsGainAndDelay() (release store on
        // m_outputArraysDirty). The main thread reads the committed copies when
        // the flag is raised (acquire load). One writer / one reader — no mutex.
        // ============================================================================

        /** Committed delay snapshot — written atomically by audio thread */
        float m_committedWfsDelays[MAX_VIRTUAL_SPEAKERS];
        /** Committed gain snapshot — written atomically by audio thread */
        float m_committedWfsLinGains[MAX_VIRTUAL_SPEAKERS];

        /**
         * Raised by audio thread after memcpy into committed arrays (release).
         * Cleared by main thread after reading committed arrays (release).
         */
        std::atomic<bool> m_outputArraysDirty{false};
        
        
        /** Virtual speaker positions  [speaker][xyz] */
        float m_virtualSpeakerPositions[MAX_VIRTUAL_SPEAKERS][3];
        /** Virtual speaker rotations  [speaker][xyz] */
        float m_virtualSpeakerRotations[MAX_VIRTUAL_SPEAKERS][3];
        /** Virtual speaker forward vectors  [speaker][xyz] */
        float m_virtualSpeakerForwards[MAX_VIRTUAL_SPEAKERS][3];

        /** WFS linear gains per output channel */
        float m_wfsLinGains[MAX_VIRTUAL_SPEAKERS];
        /** WFS delays per output channel (seconds) */
        float m_wfsDelays[MAX_VIRTUAL_SPEAKERS];
        
        /** If true, per-speaker WFS amplitude weights are computed and applied (cos(phi)/sqrt(r)).
         *  If false, all speaker gains are set to 1.0 (delay-only mode).
         *  Written from the main thread, read from the audio thread → atomic. */
        std::atomic<bool> m_isWfsGain { false };

        /** When true, min/max delay reference for time reversal is computed only
         *  over speakers with mask > 0.5. Prevents L/R inversion on focused sources. */
        std::atomic<bool> m_isActiveSpeakersMinMax { false };

        /**
         * @brief Effective radius (metres) of the WFS secondary sources (loudspeakers).
         *
         * Used by P1 (amplitude regularisation) and P2 (mask taper).
         * Written from main thread, read from audio thread → atomic<float>.
         * 0 = ideal point sources (original behaviour). Default: 0.3 m.
         */
        std::atomic<float> m_secondarySourceSize { 0.3f };

        /** Per-speaker smoothed WFS linear gains.
         *  Pre-allocated to MAX_VIRTUAL_SPEAKERS to avoid any audio-thread allocation.
         *  Smoothing prevents amplitude clicks when source or speaker positions change. */
        juce::LinearSmoothedValue<float> m_wfsGainSmoothers[MAX_VIRTUAL_SPEAKERS];

        /** WFS speaker activation mask */
        float m_wfsSpeakerMask[MAX_VIRTUAL_SPEAKERS];
        /** Temporary work buffer for outside-mask calculation */
        float m_workBufferOutsideMask[MAX_VIRTUAL_SPEAKERS];

        /** Smoothed speaker mask per output channel */
        juce::LinearSmoothedValue<float> m_wfsSpeakerMaskSmoother[MAX_VIRTUAL_SPEAKERS];
        
        float m_attenuation;
        float m_minDistance;
        float m_wfsMinDelay;
        float m_wfsMaxDelay;
        
        float m_sourcePosX;
        float m_sourcePosZ;
        float m_listenerPosX;
        float m_listenerPosZ;
        float m_sourceRotation;
        
        int m_numActiveSpeakerInMask;
        
        bool m_isWfsSpeakerMask;
        
        juce::LinearSmoothedValue<float> m_sourcePosXSmoother;
        juce::LinearSmoothedValue<float> m_sourcePosZSmoother;
    };
}
