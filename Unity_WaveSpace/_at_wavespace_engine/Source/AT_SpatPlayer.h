/**
 * @file AT_SpatPlayer.h
 * @brief Audio player class for loading and playing audio files with dynamic file path updates
 * @author Antoine Gonot
 * @date 2025
 */

#pragma once

#include <JuceHeader.h>
#include "AT_Spatializer.h"
#include "AT_BinauralSimpleSpatializer.h"
#include "AT_WfsPrefilter.h"
#include "AT_SixDofMaskProcessor.h"

namespace AT
{
    /**
     * @class SpatPlayer
     * @brief Manages audio file playback with transport controls and resampling capabilities
     * 
     * This class encapsulates audio file loading, playback control, and real-time resampling.
     * Each player instance has a unique identifier and can handle multi-channel audio files.
     * The audio file path can be set after construction using setFilepath(), allowing for
     * dynamic file switching during the lifetime of the player.
     */
    class SpatPlayer
    {
    public:
        /**
         * @brief Constructs a new SpatPlayer instance without loading an audio file
         * @param numOutputChannels Number of output channels in the audio device
         * @param is3D flag telling if the player is 3D (spatialized) or 2D (not spatialized)
         * @param isLooping flag telling if the player is looping the audio file

         * Creates a player instance with the specified output channel configuration.
         * The audio file to play must be set using setFilepath() before starting playback.
         * Automatically assigns a unique ID to this player instance.
         */
        explicit SpatPlayer(int numOutputChannels, bool is3D, bool isLooping);
        
        /**
         * @brief Destructs the SpatPlayer instance
         */
        ~SpatPlayer();
        
        /**
         * @brief Sets or changes the audio file path and loads the file
         * @param path Full path to the audio file to load
         * @return True if the file was loaded successfully, false otherwise
         * 
         * This method can be called multiple times to switch between different audio files.
         * If an audio file is already loaded, it will be properly released before loading
         * the new file.
         * 
         * IMPORTANT: This method automatically stops playback before changing the file
         * to ensure thread safety. You do not need to call stop() before calling this method.
         * 
         * If prepareToPlay() has already been called on this player, the new audio source
         * will be automatically prepared with the existing audio settings.
         * 
         * @note This method should NOT be called from the audio processing thread
         * @note Playback is automatically stopped when changing the file
         */
        bool setFilePath(juce::String path);
        
        /**
         * @brief Starts audio playback
         * 
         * If no audio file has been set via setFilePath(), this method will have no effect.
         */
        void start();
        
        /**
         * @brief Checks if the player is currently playing
         * @return True if playing, false otherwise
         */
        bool isPlaying() const;
        
        /**
         * @brief Stops audio playback
         */
        void stop();

        /**
         * @brief Click-free stop: fades the OUTPUT to silence over one audio block,
         *        then calls stop().
         *
         * Unlike stop(), this method applies the fade AFTER all spatial processing
         * (delay line output for WFS, convolution tail for binaural), which is the
         * only correct point where an abrupt cut can be avoided.
         *
         * Internally: sets m_startFadeRequest (atomic), then blocks on
         * m_fadeCompletedEvent until the audio thread has completed the ramp and
         * signalled completion. Only then calls stop() — guaranteeing that the last
         * sample written to the output buffer is silence.
         *
         * Must NOT be called from the audio thread.
         */
        void stopWithFade();

        /**
         * @brief Sets the output fade-in duration used when start() is called.
         * Thread-safe (atomic); picked up by the audio thread when the deferred
         * fade-in actually begins. Values < 0 are clamped to 0.
         */
        void setFadeInDuration(float seconds);

        /// Returns the current fade-in duration in seconds (thread-safe).
        float getFadeInDuration() const;
        
        /**
         * @brief Sets the gain of the audiofile
         * @param gain gain of the audiofile
         */
        void setGain(float gain);
        
        /**
         * @brief Sets the playback speed/rate
         * @param playbackSpeed Playback speed multiplier (1.0 = normal speed)
         */
        void setPlaybackSpeed(float playbackSpeed);
        
        /**
         * @brief Enables or disables looping playback
         * @param isLooping True to enable looping, false to play once
         */
        void setLooping(bool isLooping);
        
        /**
         * @brief Prepares the player for audio processing
         * @param samplesPerBlock Number of samples per audio block
         * @param sampleRate Target sample rate in Hz
         * 
         * If an audio file has been loaded via setFilepath(), it will be prepared
         * with these audio settings. If called before setFilepath(), the settings
         * are stored and will be applied when a file is loaded.
         */
        void prepareToPlay(int samplesPerBlock, double sampleRate);
        
        /**
         * @brief Releases all audio resources
         */
        void releaseResources();
        
        /**
         * @brief Updates the internal buffer with the next audio block
         * 
         * Must be called before accessing the buffer with getBuffer().
         * If no audio file is loaded, this method does nothing.
         */
        void updateForNextBlock();
        
        /**
         * @brief Gets a pointer to the audio buffer for a specific channel
         * @param channel Channel index (0-based)
         * @return Pointer to the audio samples for the requested channel, or nullptr if no file loaded
         */
        float* getBuffer(int channel);
        
        /**
         * @brief Gets a sample from audio buffer for a specific channel and sample index
         * @param channel Channel index (0-based)
         * @param sampleIndex Sample index (0-based)
         * @return Audio sample, or 0.0f if no file is loaded or indices are invalid
         */
        float getSample(int channel, int sampleindex);
        
        /**
         * @brief process the first channel of audio fil with delay (and eventually gain)  of the driving function, and add the samples to the samples for each channel of the output buffer to fill
         * @param bufferToFill the output buffer to fill
         * @param listenerX/Y/Z Current (smoothed) listener position, engine/world frame.
         *        Only consumed by the 2D fast path when 6DOF source masking is enabled
         *        (see AT_SixDofMaskProcessor) — ignored otherwise. Passed in rather than
         *        queried via an engine back-pointer because 2D players hold no such
         *        pointer (only m_puSpatializer, 3D-only, exposes one).
         */
        void processAndAdd(const juce::AudioSourceChannelInfo& bufferToFill,
                            float listenerX = 0.0f, float listenerY = 0.0f, float listenerZ = 0.0f);
                
        /**
         * @brief Gets the unique identifier for this player
         * @return Unique player ID
         */
        int getUID() const;
        
        /**
         * @brief Sets the unique identifier for this player
         * @return Unique player ID
         */
        void setUID(int uid);
        /**
         * @brief Checks if an audio file is currently loaded
         * @param uid unique id
         */
        bool hasFileLoaded() const;
        
        /**
         * @brief Gets the 2D/3D state
         * 
         * 3D means spatializing the playback audio through all output channels.
         * 2D means copying player channels to output channels.
         * 
         * @return True if 3D, false otherwise
         */
        bool getIs3D();
        
        /**
         * @brief Sets the 2D/3D state
         * 
         * If the state changes from false to true, instantiates the Spatializer object.
         * If the state changes from true to false, releases the Spatializer object.
         *
         * Warning: Instantiating/releasing the Spatializer object could involve memory
         * management operations (e.g., juce::dsp::DelayLine). Avoid using this in the
         * audio processing callback method.
         * 
         * @param is3D True for 3D spatialization, false for 2D
         */
        void setIs3D(bool is3D);
        
        /**
         * @brief Sets the "pre-filtering" / "not pre-filtering" state
         *
         * @param isPrefilter True if the pre-filtering is applied
         */
        void setIsPrefilter(bool isPrefilter);
        
        /**
         * @brief get a pointer to the Spatializer instance
         *
         * @return pointer to the Spatializer instance
         */
        AT::Spatializer*  getSpatializer();

        // ====================================================================
        // 6DOF SOURCE MASKING (2D players only)
        //
        // Listener-position-dependent per-channel gain, applied to the raw
        // captured signal already in the 2D player's buffer — no separate
        // geometry needed, the player's N channels are assumed to match the
        // engine's virtual speaker positions 1:1 (same convention as WFS).
        // See AT_SixDofMaskProcessor.h for the algorithm.
        // ====================================================================

        /**
         * @brief Enables or disables 6DOF source masking for this player.
         *
         * Lazily constructs the SixDofMaskProcessor on first enable (mirrors
         * the m_puSpatializer lazy-construction pattern in setIs3D()) — NOT
         * real-time safe, must not be called from the audio thread. Has no
         * audible effect while m_is3D is true (masking only applies to the
         * 2D fast path in processAndAdd()).
         */
        void setIs6dofMaskEnabled(bool isEnabled);
        bool getIs6dofMaskEnabled() const;

        /// Number of sources the MUSIC signal subspace is sized for / peak
        /// search cap. Thread-safe.
        void set6dofMaxSources(int maxSources);
        int get6dofMaxSources() const;

        /// Grid resolution (metres) for the 6DOF TEMPORAL mode-detection
        /// histogram — distinct from the MUSIC spatial search grid, see
        /// set6dofSearchGridResolution(). Thread-safe.
        void set6dofGridRes(float gridRes);
        float get6dofGridRes() const;

        /// Minimum matching-estimate count for a 6DOF source to be accepted. Thread-safe.
        void set6dofMinBlockCount(int minBlockCount);
        int get6dofMinBlockCount() const;

        /// Number of audio blocks buffered into one 6DOF localization analysis window. Thread-safe.
        void set6dofNumBufferedBlocks(int numBufferedBlocks);
        int get6dofNumBufferedBlocks() const;

        /// Spatial resolution (metres) of the MUSIC candidate-position search grid. Thread-safe.
        void set6dofSearchGridResolution(float searchGridResolution);
        float get6dofSearchGridResolution() const;

        /// Number of frequency bands combined per MUSIC analysis window. Thread-safe.
        void set6dofMaxBins(int maxBins);
        int get6dofMaxBins() const;

        /// Lower bound (Hz) of the frequency range analyzed by MUSIC. Thread-safe.
        void set6dofBandHzMin(float bandHzMin);
        float get6dofBandHzMin() const;

        /// Upper bound (Hz) of the frequency range analyzed by MUSIC. Thread-safe.
        void set6dofBandHzMax(float bandHzMax);
        float get6dofBandHzMax() const;

        /// Lower bound (metres) of the height range swept by the MUSIC search grid. Thread-safe.
        void set6dofYRangeMin(float yRangeMin);
        float get6dofYRangeMin() const;

        /// Upper bound (metres) of the height range swept by the MUSIC search grid. Thread-safe.
        void set6dofYRangeMax(float yRangeMax);
        float get6dofYRangeMax() const;

        /**
         * @brief Feeds the 6DOF mask processor with the array geometry it needs
         *        (mic positions = virtual speaker positions, same convention
         *        as WFS) and finalizes its allocation.
         *
         * Called by SpatializationEngine right after setIs6dofMaskEnabled(true)
         * — SpatPlayer holds no back-pointer to the engine (2D players never
         * needed one before), so the engine pushes its own
         * m_virtualSpeakerPositionsFlat down explicitly instead. NOT
         * real-time safe (allocates) — same contract as setIs6dofMaskEnabled().
         *
         * @param speakerPositionsFlat  Engine's [x0,y0,z0,x1,y1,z1,...] array.
         * @param numSpeakerPositions   Length of that array / 3.
         */
        void prepare6dofMask(double sampleRate, int maxBlockSize,
                              const float* speakerPositionsFlat, int numSpeakerPositions);

        /// Number of currently detected 6DOF sources (0 if disabled/not yet detected).
        int get6dofNumDetectedSources() const;

        /**
         * @brief Copies detected 6DOF source positions into a flat [x,y,z,...] array.
         * @return Number of sources actually copied.
         */
        int get6dofSourcePositions(float* outPositions, int maxSources) const;
        
        /**
         * @brief get the RMS level of the played audio file in the player
         *
         * @param uid Unique identifier of the player
         * @param meters array of RMS value of the channel of the audiofile
         */
        void getMeters(float* meters, int arraySize);
        
        /**
         * @brief Gets the number of channels in the loaded audio file
         * @param numChannel Number of audio channels, or 0 if no file is loaded
         */
        void getNumChannel(int* numChannel);
        
        /**
         * @brief Set simple binaural spatialization mode
         * @param isSimple True for simple binaural, false for WFS mode
         *
         * When true and m_is3D is true, getSample() will use simple binaural
         * panning instead of WFS spatialization.
         */
        void setIsSimpleBinauralSpat(bool isSimple);
        
        /**
         * @brief Initialize simple binaural spatializer with shared HRTF processor
         * @param ownedProcessor owned HRTF processor from SpatializationEngine
         * @param sampleRate Sample rate in Hz
         *
         * Must be called during setup if binaural virtualization is enabled.
         * The HRTF processor is NOT owned by SpatPlayer.
         */
        void initializeSimpleBinaural(std::unique_ptr<HRTFProcessor> ownedProcessor, double sampleRate);
        
        HRTFProcessor* getOwnedHrtfProcessor() const;
        
        /**
         * @brief Pre-warms the binaural convolver with the correct IR for current position.
         * Call at the start of fade-out so async IR load completes before fade-in.
         */
        void preWarmBinaural();
        
    private:
        /**
         * @brief Internal method to initialize the audio source from the current file path
         * @return True if initialization succeeded, false otherwise
         * 
         * This method handles the creation of the AudioFormatReaderSource and
         * configuration of the transport source.
         */
        bool initializeAudioSource();
        
        /**
         * @brief Internal method to release the current audio source
         * 
         * Safely stops and releases all resources associated with the current audio file.
         */
        void releaseAudioSource();
        
        /**
         * @brief Audio format manager for reading various audio file formats
         */
        juce::AudioFormatManager m_formatManager;
        
        /**
         * @brief Reader source for the audio file
         */
        std::unique_ptr<juce::AudioFormatReaderSource> m_upReaderSource;
        
        /**
         * @brief Transport source for playback control
         */
        juce::AudioTransportSource m_transportSource;
        
        /**
         * @brief Resampling source for variable playback speed
         * Using unique_ptr to allow dynamic recreation with correct channel count
         */
        std::unique_ptr<juce::ResamplingAudioSource> m_puResampleSource;
        
        /**
         * @brief Temporary buffer for audio processing
         */
        std::unique_ptr<juce::AudioBuffer<float>> m_puTempBuffer;
        
        /**
         * @brief Audio source channel info wrapper for the buffer
         */
        std::unique_ptr<juce::AudioSourceChannelInfo> m_puAsci;
        
        /**
         * @brief File object representing the loaded audio file
         */
        juce::File m_audioFile;
        
        /**
         * @brief Path to the audio file
         */
        juce::String m_path;
        
        /**
         * @brief Spatializer object used to spatialize SpatPlayer audio if needed
         */
        std::unique_ptr<AT::Spatializer> m_puSpatializer;

        /**
         * @brief 6DOF source-masking processor (2D players only). Null until
         * setIs6dofMaskEnabled(true) is called at least once — same lazy
         * lifecycle contract as m_puSpatializer for 3D mode.
         */
        std::unique_ptr<AT::SixDofMaskProcessor> m_puSixDofMask;
        bool m_is6dofMaskEnabled = false;

        /**
         * @brief True once prepare6dofMask() has successfully allocated the
         * processor for the CURRENT enable cycle. Guards against re-running
         * SixDofMaskProcessor::prepare() (full realloc + background-thread
         * restart, unsynchronized with the audio thread mid-flight) on every
         * redundant call — e.g. a caller pushing "isEnabled=true" every audio
         * callback instead of only on the actual OFF->ON transition. Reset to
         * false in setIs6dofMaskEnabled(false), so a genuine re-enable (or a
         * new file with a different channel count) still re-prepares.
         */
        bool m_is6dofMaskPrepared = false;
        
        /**
        * @brief Simple binaural spatializer (used when m_isSimpleBinauralSpat = true)
        * Shares HRTF processor with SpatializationEngine's binaural virtualization
        */
       std::unique_ptr<BinauralSimpleSpatializer> m_puBinauralSimpleSpatializer;

        /**
         * @brief array of instant RMS value (in decibels) of the channels of the audiofile for a given sample block
         */
        std::unique_ptr<float[]> m_puMeters;
        
        /**
         * @brief Number of channels in the audio file
         */
        int m_numChannel = 0;
        
        /**
         * @brief Number of output channels in the audio device
         */
        int m_numOutputChannels = 0;
        
        /**
         * @brief Number of samples per processing block
         */
        int m_samplesPerBlock = 0;
        
        /**
         * @brief Sample rate in Hz
         */
        double m_sampleRate = 0.0;
        
        /**
         * @brief Current gain (dB)
         */
        float m_gain = 0.0f;

        /**
         * @brief Linear gain cached from m_gain (dB). Updated once in setGain().
         *
         * Avoids recomputing std::pow(10, gain/20) on every sample inside
         * updateForNextBlock(). Initialised to 1.0 (0 dB).
         */
        float m_linearGain = 1.0f;

        /**
         * @brief Pre-allocated per-sample gain ramp buffer for 2D fade-out.
         *
         * Sized to m_samplesPerBlock in prepareToPlay(). The ramp (1.0 → 0.0)
         * is written once per fading block and then used as the per-element
         * multiplier in FloatVectorOperations::addWithMultiply(), which maps
         * to SSE/AVX/NEON on supported platforms.
         *
         * Never reallocated in the audio thread.
         */
        std::unique_ptr<float[]> m_puGainRamp;

        /**
         * @brief Current playback speed multiplier
         */
        float m_playbackSpeed = 1.0f;
        
        /**
         * @brief Looping state flag
         */
        bool m_isLooping = false;
        
        /**
         * @brief 2D/3D state flag
         */
        bool m_is3D = false;
        
        
        
        /**
         * @brief "pre-filtering" / "not pre-filtering" state flag
         */
        bool m_isPrefilter = false;
        
        /**
         * @brief Flag indicating if prepareToPlay has been called
         */
        bool m_isPrepared = false;
        
        /**
         * @brief Unique identifier for this player instance
         */
        int m_uid;
        
        /**
         * @brief Static counter for generating unique IDs
         */
        static int m_numInstances;
        
        // ============================================================================
        // SIMPLE BINAURAL
        // ============================================================================
        /**
         * @brief Flag for simple binaural mode (set by SpatializationEngine)
         * When true, getSample() uses BinauralSimpleSpatializer instead of WFS
         */
        bool m_isSimpleBinauralSpat = false;
        /**
         * @brief owned HRTFProcessor given by the SpatializationEngine
         */
        std::unique_ptr<HRTFProcessor> m_puOwnedHrtfProcessor;
        
        // ============================================================================
        // CLICK-FREE STOP — output fade members
        //
        // The fade is applied at the OUTPUT of processAndAdd() (post delay-line for
        // WFS, post-convolution tail for binaural). This is the only correct point:
        // fading the INPUT leaves the delay line / convolver tail at full amplitude.
        //
        // Thread model:
        //   Main thread  → m_startFadeRequest (atomic write)
        //   Audio thread → reads m_startFadeRequest, owns m_isFadingOut / m_fadeGain
        //                  / m_fadeStep, signals m_fadeCompletedEvent when done
        //   Main thread  → waits on m_fadeCompletedEvent, then calls stop()
        // ============================================================================

        /// Set by main thread (stopWithFade) to trigger the ramp on the next audio block.
        std::atomic<bool> m_startFadeRequest{false};

        /// Signalled by the audio thread once the fade ramp has finished.
        /// Main thread waits on this before calling stop().
        juce::WaitableEvent m_fadeCompletedEvent;

        /// True while the fade ramp is in progress (audio thread only).
        bool m_isFadingOut = false;

        /// True while the start fade-in ramp is in progress (audio thread only).
        bool m_isFadingIn = false;

        /// Set by main thread (start()) to trigger the fade-in on the next audio
        /// block — same cross-thread pattern as m_startFadeRequest. The audio
        /// thread defers consuming it until the engine has adopted a real
        /// listener transform (see processAndAdd()).
        std::atomic<bool> m_startFadeInRequest{false};

        /// Fade-in duration in seconds (main thread writes via setFadeInDuration,
        /// audio thread reads when the deferred fade-in actually starts).
        std::atomic<float> m_fadeInDurationSeconds{0.3f};

        /// Current per-sample output multiplier during fade (audio thread only, 1→0).
        float m_fadeGain = 1.0f;

        /// Amount subtracted from m_fadeGain each sample (audio thread only).
        float m_fadeStep = 0.0f;

        // ============================================================================
        // DISTANCE ATTENUATION SMOOTHING
        // ============================================================================

        /**
         * @brief Per-sample smoother for the distance-attenuation gain.
         *
         * computeDistanceGain() (1/d^attenuation, with a pow()) is too expensive
         * to evaluate per sample, so it is evaluated once per block — but applying
         * that value as a CONSTANT over the whole block makes the gain move in
         * per-block STAIRCASE steps during listener/source translation. At 2048
         * samples per block, those steps repeat at the block rate (~23 Hz @ 48 kHz)
         * and are audible as periodic clicks — the strongest remaining click
         * source in translation after all positions/masks/gains were smoothed.
         *
         * Fix: the block-rate evaluation becomes the smoother's TARGET, and the
         * applied gain ramps toward it per sample (20 ms ramp). All three output
         * paths (Simple Binaural, 2D, WFS/3D) consume this smoother.
         * Audio thread only.
         */
        juce::LinearSmoothedValue<float> m_distanceGainSmoother { 1.0f };

        /// One-shot: snap the smoother on the first block (same rationale as the
        /// first-listener-transform snaps — no ramp from a meaningless default).
        bool m_distanceGainSnapPending = true;

        // ============================================================================
        // IIR WFS PREFILTER  —  H(omega) = sqrt(j*omega)
        //
        // Approximates the 2.5D WFS driving function half-derivative using a cascade
        // of NUM_SECTIONS first-order IIR sections with logarithmically spaced
        // pole-zero pairs. Coefficients are computed analytically at prepare() time
        // from the sample rate — no external coefficient files are required.
        //
        // Applied on channel 0 only, before the sample is pushed into m_wfsDelayLine.
        // Reset whenever the prefilter is toggled to avoid a transient from stale state.
        // ============================================================================
        AT::WfsPrefilter m_wfsPrefilter;
        

    };
}
