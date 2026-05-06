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
         */
        void processAndAdd(const juce::AudioSourceChannelInfo& bufferToFill);
                
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

        /// Current per-sample output multiplier during fade (audio thread only, 1→0).
        float m_fadeGain = 1.0f;

        /// Amount subtracted from m_fadeGain each sample (audio thread only).
        float m_fadeStep = 0.0f;

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
