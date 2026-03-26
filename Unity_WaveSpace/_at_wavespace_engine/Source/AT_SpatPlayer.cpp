/**
 * @file AT_SpatPlayer.cpp
 * @brief Implementation of the SpatPlayer class with dynamic file path management
 * @author Audio Technology Team
 * @date 2025
 */

#pragma once

#include "AT_SpatPlayer.h"
#include "AT_SpatializationEngine.h"
#include "UnityLogger.h"
#include "AT_SpatConfig.h"

namespace AT
{
    // Static member initialization - tracks total number of player instances created
    int SpatPlayer::m_numInstances = 0;

    SpatPlayer::SpatPlayer(int numOutputChannels, bool is3D, bool isLooping)
        : m_numOutputChannels(numOutputChannels)          // Initialize the number of output channels
        , m_is3D(is3D)                                   // 2D mode by default
        , m_isLooping(isLooping)                          // Looping disabled by default
        , m_playbackSpeed(1.0f)                           // Default playback at normal speed
        , m_isPrepared(false)                             // Not yet prepared
        , m_numChannel(0)                                 // Channel count will be determined from file
        , m_samplesPerBlock(0)                            // Will be set in prepareToPlay()
        , m_sampleRate(0.0)                               // Will be set in prepareToPlay()
        , m_isSimpleBinauralSpat(false)                   // Simple binaural mode disabled by default
    {
                
        // Register common audio formats (WAV, AIFF, MP3, OGG, FLAC, etc.)
        m_formatManager.registerBasicFormats();
        
        m_numInstances++;
        
    }

    SpatPlayer::~SpatPlayer(){
        m_numInstances--;
    }

    bool SpatPlayer::setFilePath(juce::String path)
    {
        // Automatically stop playback before changing the file.
        stop();
        releaseAudioSource();
        
        m_path = path;
        m_audioFile = juce::File(path);
        m_numChannel = 0;
        
        if (m_audioFile == juce::File {} || !m_audioFile.existsAsFile())
            return false;
        
        if (!initializeAudioSource())
            return false;
        
        // If already prepared, recreate buffers for the new file's channel count.
        if (m_isPrepared && m_sampleRate > 0 && m_samplesPerBlock > 0)
        {
            m_puTempBuffer = std::make_unique<juce::AudioBuffer<float>>(m_numChannel, m_samplesPerBlock);
            m_puAsci = std::make_unique<juce::AudioSourceChannelInfo>(m_puTempBuffer.get(), 0, m_samplesPerBlock);
            m_transportSource.prepareToPlay(m_samplesPerBlock, m_sampleRate);
            m_puResampleSource->prepareToPlay(m_samplesPerBlock, m_sampleRate);
        }
        
        return true;
    }

    bool SpatPlayer::initializeAudioSource()
    {
        auto* reader = m_formatManager.createReaderFor(m_audioFile);
        
        if (reader == nullptr)
            return false;
        
        // The second parameter (true) means the source owns and will delete the reader.
        auto source = std::make_unique<juce::AudioFormatReaderSource>(reader, true);
        
        m_numChannel = static_cast<int>(reader->numChannels);
                
        // Connect the reader source to the transport source
        // Parameters: source, readAheadBufferSize (0 = no buffering), backgroundThread (nullptr), sourceSampleRate, max number of channels
        m_transportSource.setSource(source.get(), 0, nullptr, reader->sampleRate, m_numChannel);
        
        // Initialize meters array.
        if (m_puMeters != nullptr) m_puMeters.reset();
        m_puMeters = std::make_unique<float[]>(m_numChannel);
        for(int i=0;i<m_numChannel; i++){
            m_puMeters[i] = -90.0f;
        }

        m_upReaderSource = std::move(source);
        
        // ResamplingAudioSource must be created after channel count is known.
        m_puResampleSource = std::make_unique<juce::ResamplingAudioSource>(
            &m_transportSource, 
            false,  // deleteSourceWhenDeleted
            m_numChannel
        );
        
        m_upReaderSource->setLooping(m_isLooping);
        m_puResampleSource->setResamplingRatio(m_playbackSpeed);
        
        return true;
    }

    void SpatPlayer::releaseAudioSource()
    {
        m_transportSource.setSource(nullptr);
        m_puResampleSource.reset();
        m_upReaderSource.reset();
        m_numChannel = 0;
    }

    void SpatPlayer::start()
    {
        if (m_upReaderSource != nullptr)
        {
            // Reset fade state before (re)starting — m_fadeGain may be 0 after stopWithFade().
            m_fadeGain          = 1.0f;
            m_isFadingOut       = false;
            m_startFadeRequest.store(false, std::memory_order_release);
            m_transportSource.start();
        }
    }
    
    void SpatPlayer::stop()
    {
        m_transportSource.stop();
    }

    void SpatPlayer::stopWithFade()
    {
        // If not playing, just call stop() immediately — no fade needed.
        if (!isPlaying())
        {
            stop();
            return;
        }

        // Signal the audio thread to begin the output fade on its next block.
        // The audio thread owns m_isFadingOut / m_fadeGain; we only set the
        // atomic request flag here.
        m_startFadeRequest.store(true, std::memory_order_release);

        // Block until the audio thread has completed the ramp and signalled us.
        // Timeout: 500 ms (>> one block duration, even at very small buffer sizes).
        // If the timeout fires (e.g. audio device closed), we fall through to stop()
        // which is safe regardless.
        bool faded = m_fadeCompletedEvent.wait(500);
        (void)faded; // stop() is called unconditionally below

        // The last sample written to the output was silence. stop() is safe now.
        stop();
    }
    
    int SpatPlayer::getUID() const
    {
        // Return the unique identifier for this player
        return m_uid;
    }

    void SpatPlayer::setUID(int uid){
        m_uid = uid;
    }

    void SpatPlayer::updateForNextBlock()
    {
        
        // Only update if we have a valid audio source and buffer
        if (m_upReaderSource != nullptr && m_puAsci != nullptr && m_puResampleSource != nullptr)
        {
            // Request the next block of audio from the resampling source
            // And apply gain on source
            m_puResampleSource->getNextAudioBlock(*m_puAsci);
            
            // Calculate RMS meters for each channel of the audio file
            // This follows the same logic as AT_SpatializationEngine::getNextAudioBlock()
            if (m_puMeters != nullptr && m_puAsci->buffer != nullptr && m_numChannel > 0)
            {
                for (int channel = 0; channel < m_numChannel; ++channel)
                {
                    if (channel < m_numChannel)
                    {
                        float* channelData = m_puAsci->buffer->getWritePointer(channel, m_puAsci->startSample);
                        
                        if (channelData != nullptr)
                        {
                            // Calculate sum of squares for RMS calculation
                            float sumOfSquares = 0.0f;
                            
                            for (int sample = 0; sample < m_puAsci->numSamples; ++sample)
                            {
                                
                                // Apply Gain on the audio file
                                channelData[sample] *= std::pow(10.0f, m_gain/20.0f);
                                
                                // if  m_isPrefilter == true and the player is 3D, then apply pre-filtering only on the first channel
                                if (m_isPrefilter && m_is3D && channel == 0) {
                                    // Apply the half-derivative prefilter (sqrt(j*omega)) to channel 0 only.
                                    // The prefilter corrects the +3 dB/octave amplitude error inherent in 2.5D
                                    // WFS synthesis. It must be applied before the sample enters m_wfsDelayLine
                                    // so that all virtual speaker outputs receive the corrected waveform.
                                    channelData[sample] = m_wfsPrefilter.processSample(channelData[sample]);
                                }
                                
                                sumOfSquares += channelData[sample] * channelData[sample];
                            }
                            
                            // Calculate RMS (Root Mean Square)
                            float rms = std::sqrt(sumOfSquares / static_cast<float>(m_puAsci->numSamples));
                            
                            // Convert RMS to decibels (dB)
                            if (rms > 0.00001f)  // Threshold to avoid log(0)
                            {
                                m_puMeters[channel] = 20.0f * std::log10f(rms);
                                
                                // Clamp to reasonable range [-90, 0] dB
                                if (m_puMeters[channel] < -90.0f) m_puMeters[channel] = -90.0f;
                                if (m_puMeters[channel] > 0.0f) m_puMeters[channel] = 0.0f;
                            }
                            else
                            {
                                m_puMeters[channel] = -90.0f;  // Silence
                            }
                        }
                        else
                        {
                            m_puMeters[channel] = -90.0f;  // No data available
                        }
                    }
                }
            }
        }
    }
    
    float* SpatPlayer::getBuffer(int channel)
    {
        // Return nullptr if no buffer is available or no file is loaded
        if (m_puAsci == nullptr || m_puAsci->buffer == nullptr || m_upReaderSource == nullptr)
        {
            return nullptr;
        }
        
        // Get a write pointer to a specific channel in our temporary buffer
        // This allows the caller to read/mix the audio samples
        return m_puAsci->buffer->getWritePointer(channel, m_puAsci->startSample);        
    }

    float SpatPlayer::getSample(int channel, int sampleIndex)
    {
        float sample = 0.0f;
        
        // Check if we have a valid buffer, file loaded, and valid indices
        if (m_puAsci == nullptr || 
            m_puAsci->buffer == nullptr || 
            m_upReaderSource == nullptr ||
            sampleIndex >= m_samplesPerBlock)
        {
            return sample;  // Return silence
        }
        
        if (!m_is3D)
        {
            // 2D mode: return the sample from the specified channel
            if (channel < m_numChannel)
            {
                sample = m_puAsci->buffer->getWritePointer(channel, m_puAsci->startSample)[sampleIndex];
            }
        }
        else
        {
            // 3D mode: spatialize the first channel across all outputs
            if (m_puSpatializer != nullptr)
            {
                // Determine if we should update the read pointer
                // Only update for the last channel to advance the delay line properly
                bool updateReadPointer = (channel == m_numOutputChannels - 1);
                
                // Call spatialize with the updateReadPointer flag
                // Note: inputSample parameter is not used in spatialize() 
                // since we already pushed the sample in processAndAdd()
                sample = m_puSpatializer->spatialize(0.0f, channel, updateReadPointer);
            }
        }
        
        return sample;
    }

    void SpatPlayer::processAndAdd(const juce::AudioSourceChannelInfo& bufferToFill) {

        // Output fade for click-free stop (triggered by stopWithFade()).
        // Applied at the OUTPUT stage — fading the input would leave the WFS
        // delay-line / HRTF convolver tail at full amplitude.
        if (m_startFadeRequest.exchange(false, std::memory_order_acq_rel))
        {
            m_isFadingOut = true;
            m_fadeGain    = 1.0f;
            m_fadeStep    = (bufferToFill.numSamples > 0)
                            ? 1.0f / static_cast<float>(bufferToFill.numSamples)
                            : 1.0f;
        }

        // Update spatial parameters once per block.
        if (m_puSpatializer != nullptr) {
            m_puSpatializer->updateSourceParametersTarget();
        }

        // Simple binaural mode: process the entire block at once.
        if (m_is3D && m_isSimpleBinauralSpat && m_puBinauralSimpleSpatializer)
        {
            // Simple binaural: process the full block through BinauralSimpleSpatializer.
            const float* monoInput = nullptr;
            if (m_puAsci != nullptr && m_puAsci->buffer != nullptr &&
                m_upReaderSource != nullptr && m_numChannel > 0)
            {
                monoInput = m_puAsci->buffer->getReadPointer(0, m_puAsci->startSample);
            }
            
            if (!monoInput)
                return;  // No input available
            
            // Get source position (raw from Spatializer - set per-player)
            float sourceX = 0.0f, sourceY = 0.0f, sourceZ = 0.0f;
            float listenerX = 0.0f, listenerY = 0.0f, listenerZ = 0.0f;
            float forwardX = 0.0f, forwardY = 0.0f, forwardZ = 0.0f;
            
            if (m_puSpatializer)
            {
                // Source position: raw from Spatializer (set via setPlayerTransform)
                m_puSpatializer->getSourcePosition(sourceX, sourceY, sourceZ);
                
                // Listener position and forward: use SMOOTHED values from SpatializationEngine
                auto* engine = m_puSpatializer->getSpatializationEngine();
                
                if (engine)
                {
                    engine->getSmoothedListenerPosition(listenerX, listenerY, listenerZ);
                    engine->getSmoothedListenerForward(forwardX, forwardY, forwardZ);
                }
                else
                {
                    // Fallback: use raw values from Spatializer
                    m_puSpatializer->getListenerPosition(listenerX, listenerY, listenerZ);
                    m_puSpatializer->getListenerForward(forwardX, forwardY, forwardZ);
                }
            }
            
            m_puBinauralSimpleSpatializer->processBlock(
                monoInput,
                bufferToFill.numSamples,
                sourceX, sourceY, sourceZ,
                listenerX, listenerY, listenerZ,
                forwardX, forwardY, forwardZ
            );
            
            // Update speaker mask (ensures numActiveSpeakerInMask > 0 when starting in binaural mode).
            m_puSpatializer->setIsInsideAndUpdateSpeakerMask();
            float numActiveSpeakerInMask = m_puSpatializer->getNumActiveSpeakerInMask();
            if (numActiveSpeakerInMask == 0) numActiveSpeakerInMask = m_numOutputChannels;

            // Distance attenuation: 1 / max(d, minDist)^attenuation — computed once per block.
            const float distanceGain = m_puSpatializer->computeDistanceGain();
            
            // Add processed stereo output (channels 0 and 1) with fade gain.
            for (int sampleIndex = 0; sampleIndex < bufferToFill.numSamples; ++sampleIndex)
            {
                float fadeGain = m_fadeGain;
                if (m_isFadingOut)
                    m_fadeGain = std::max(0.0f, m_fadeGain - m_fadeStep);

                float leftSample  = m_puBinauralSimpleSpatializer->getSample(0, sampleIndex)
                                    / std::sqrt(numActiveSpeakerInMask) * fadeGain * distanceGain;
                float rightSample = m_puBinauralSimpleSpatializer->getSample(1, sampleIndex)
                                    / std::sqrt(numActiveSpeakerInMask) * fadeGain * distanceGain;
                
                if (bufferToFill.buffer->getNumChannels() > 0)
                    bufferToFill.buffer->addSample(0, bufferToFill.startSample + sampleIndex, leftSample);
                
                if (bufferToFill.buffer->getNumChannels() > 1)
                    bufferToFill.buffer->addSample(1, bufferToFill.startSample + sampleIndex, rightSample);
            }

            if (m_isFadingOut && m_fadeGain <= 0.0f)
            {
                m_isFadingOut = false;
                m_fadeCompletedEvent.signal();
            }
            
            return;
        }
        
        // WFS / 2D mode: sample-by-sample processing.
        // Distance attenuation: computed once per block from smoothed positions.
        const float distanceGain = (m_puSpatializer != nullptr)
                                   ? m_puSpatializer->computeDistanceGain()
                                   : 1.0f;

        // For 3D: address all virtual speaker slots (m_numOutputChannels = m_numVirtualSpeakers).
        // For 2D: the declared limit is m_numOutputChannels (= m_numVirtualSpeakers), NOT
        //         bufferToFill.buffer->getNumChannels() which equals the physical output
        //         channel count — only 2 in binaural mode, whereas the 2D file may have N
        //         channels intended for N virtual speakers.
        //         bufferToFill.buffer->getNumChannels() is kept as a hard safety clamp only
        //         to prevent out-of-bounds writes if the buffer is unexpectedly narrow.
        const int numChannelsToProcess = m_is3D
            ? m_numOutputChannels
            : std::min(m_numChannel,
                       std::min(m_numOutputChannels, bufferToFill.buffer->getNumChannels()));

        for (auto sampleIndex = 0; sampleIndex < bufferToFill.numSamples; sampleIndex++)
        {
            if (m_puSpatializer != nullptr)
                m_puSpatializer->advanceSourceSmoothers();
            
            // 3D / WFS mode: push the mono source sample into the delay line.
            // 2D mode does NOT use the delay line — getSample() reads directly
            // from m_puAsci->buffer, so no push is needed (and m_puSpatializer
            // is nullptr in 2D mode, so accessing it would crash).
            if (m_is3D && m_puSpatializer != nullptr &&
                m_puAsci != nullptr && m_puAsci->buffer != nullptr &&
                m_upReaderSource != nullptr)
            {
                float inputSample = m_puAsci->buffer->getWritePointer(0, m_puAsci->startSample)[sampleIndex];
                m_puSpatializer->m_wfsDelayLine.pushSample(0, inputSample);
            }
            
            for (auto outputChannel = 0; outputChannel < numChannelsToProcess; outputChannel++)
            {
                auto* outputBuffer = bufferToFill.buffer->getWritePointer(outputChannel, bufferToFill.startSample);
                outputBuffer[sampleIndex] += getSample(outputChannel, sampleIndex) * m_fadeGain * distanceGain;
            }

            // Advance fade gain once per sample (after all channels).
            if (m_isFadingOut)
                m_fadeGain = std::max(0.0f, m_fadeGain - m_fadeStep);
        }

        if (m_isFadingOut && m_fadeGain <= 0.0f)
        {
            m_isFadingOut = false;
            m_fadeCompletedEvent.signal();
        }
        
    }

    bool SpatPlayer::isPlaying() const
    {
        // Return false if no file is loaded, otherwise query the transport source
        if (m_upReaderSource == nullptr)
        {
            return false;
        }
        
        return m_transportSource.isPlaying();
    }

    bool SpatPlayer::hasFileLoaded() const
    {
        // Check if we have a valid audio source
        return m_upReaderSource != nullptr;
    }

    bool SpatPlayer::getIs3D()
    {
        return m_is3D;
    }

    void SpatPlayer::setIs3D(bool is3D)
    {
        if (m_is3D && !is3D)
        {
            // Transitioning from 3D to 2D: release spatializer
            if (m_puSpatializer != nullptr) 
            {
                m_puSpatializer.reset();
            }
        }
        else if (is3D && !m_is3D)
        {
            // Transitioning from 2D to 3D: instantiate spatializer
            // Only if we have valid audio settings
            if (m_sampleRate > 0 && m_samplesPerBlock > 0)
            {
                m_puSpatializer = std::make_unique<Spatializer>(
                    *this,
                    m_numOutputChannels, 
                    m_samplesPerBlock, 
                    m_sampleRate
                );
            }
        }
        
        m_is3D = is3D;
    }

    void SpatPlayer::setIsPrefilter(bool isPrefilter){
        // Reset filter state when toggling to avoid a transient caused by
        // stale delay registers from the previous mode.
        if (isPrefilter != m_isPrefilter)
            m_wfsPrefilter.reset();

        m_isPrefilter = isPrefilter;
    }

    void SpatPlayer::setGain(float gain){
        m_gain = gain;
    }

    void SpatPlayer::setPlaybackSpeed(float playbackSpeed)
    {
        // Store the new playback speed
        m_playbackSpeed = playbackSpeed;
        
        // Update the resampling ratio in the resampling source
        // A ratio of 1.0 = normal speed, 2.0 = double speed, 0.5 = half speed
        if (m_puResampleSource != nullptr)
        {
            m_puResampleSource->setResamplingRatio(playbackSpeed);
        }
    }
    
    void SpatPlayer::setLooping(bool isLooping)
    {
        // Store the looping state
        m_isLooping = isLooping;
        
        // Configure the reader source to loop (or not) when it reaches the end
        // Only if a file is loaded
        if (m_upReaderSource != nullptr)
        {
            m_upReaderSource->setLooping(isLooping);
        }
    }

    void SpatPlayer::prepareToPlay(int samplesPerBlock, double sampleRate)
    {
        // Store audio processing parameters for later use
        m_samplesPerBlock = samplesPerBlock;
        m_sampleRate = sampleRate;
        m_isPrepared = true;
        
        // Only prepare sources if we have a file loaded
        if (m_upReaderSource != nullptr && m_puResampleSource != nullptr)
        {
            // Prepare the transport source with the audio settings
            // This allocates internal buffers and prepares for audio processing
            m_transportSource.prepareToPlay(samplesPerBlock, sampleRate);
            
            // Prepare the resampling source with the same settings
            // This ensures the resampler is ready to process audio at the target sample rate
            m_puResampleSource->prepareToPlay(samplesPerBlock, sampleRate);
        }
        
        // Allocate a temporary buffer to hold one block of audio
        // Use the maximum of numChannel and 1 to handle the case where no file is loaded yet
        int bufferChannels = (m_numChannel > 0) ? m_numChannel : 2;
        m_puTempBuffer = std::make_unique<juce::AudioBuffer<float>>(bufferChannels, samplesPerBlock);
        
        // Create an AudioSourceChannelInfo wrapper for our buffer
        // This provides metadata about the buffer (start sample, number of samples)
        m_puAsci = std::make_unique<juce::AudioSourceChannelInfo>(m_puTempBuffer.get(), 0, samplesPerBlock);
        
        juce::dsp::ProcessSpec spec;
        spec.sampleRate = sampleRate;
        spec.maximumBlockSize = juce::uint32(samplesPerBlock);
        spec.numChannels = 1;
       
        
        // If 3D mode is enabled and we don't have a spatializer yet, create one
        if (m_is3D && m_puSpatializer == nullptr)
        {
            m_puSpatializer = std::make_unique<Spatializer>(
                *this,
                m_numOutputChannels, 
                m_samplesPerBlock, 
                m_sampleRate
            );
            
            // Design the IIR half-derivative prefilter for the current sample rate.
            // WFS synthesis is physically valid from ~50 Hz (limited by array aperture)
            // to 20 kHz. The IIR approximation is accurate to ±0.5 dB across this range
            // at any sample rate — no per-rate coefficient files are needed.
            m_wfsPrefilter.prepare(sampleRate, 50.0f, 20000.0f);
        }
    }

    void SpatPlayer::releaseResources()
    {
        // Release resources allocated by the transport source
        // This frees any internal buffers or threads
        m_transportSource.releaseResources();
        
        // Release resources allocated by the resampling source
        if (m_puResampleSource != nullptr)
        {
            m_puResampleSource->releaseResources();
        }
        
        // Mark as not prepared
        m_isPrepared = false;
    }

    AT::Spatializer* SpatPlayer::getSpatializer(){
        return m_puSpatializer.get();
    }

    void SpatPlayer::getMeters(float* meters, int arraySize){
        if (m_puMeters != nullptr && meters != nullptr && arraySize == m_numChannel){
            for(int i = 0; i < arraySize; i++){
                meters[i] = m_puMeters[i];
            }
        }    }

    void SpatPlayer::getNumChannel(int* numChannel){
        *numChannel = m_numChannel;
    }

    void SpatPlayer::setIsSimpleBinauralSpat(bool isSimple)
    {
        m_isSimpleBinauralSpat = isSimple;
                
    }

    void SpatPlayer::initializeSimpleBinaural(std::unique_ptr<HRTFProcessor> ownedProcessor, double sampleRate)
    {
        m_puOwnedHrtfProcessor = std::move(ownedProcessor); // transfer ownership of the HRTF processor
        
        if (!m_puBinauralSimpleSpatializer)
            m_puBinauralSimpleSpatializer = std::make_unique<BinauralSimpleSpatializer>();
        
        m_puBinauralSimpleSpatializer->initialize(
            m_puOwnedHrtfProcessor.get(),
            sampleRate,
            m_samplesPerBlock
        );
    }

    HRTFProcessor* SpatPlayer::getOwnedHrtfProcessor() const
    {
        return m_puOwnedHrtfProcessor.get();
    }

    void SpatPlayer::preWarmBinaural()
    {
        if (!m_puOwnedHrtfProcessor || !m_puSpatializer)
            return;

        float sourceX, sourceY, sourceZ;
        m_puSpatializer->getSourcePosition(sourceX, sourceY, sourceZ);

        float listenerX, listenerY, listenerZ;
        float forwardX, forwardY, forwardZ;
        auto* engine = m_puSpatializer->getSpatializationEngine();
        if (engine)
        {
            engine->getSmoothedListenerPosition(listenerX, listenerY, listenerZ);
            engine->getSmoothedListenerForward(forwardX, forwardY, forwardZ);
        }
        else return;

        // Same azimuth calculation as BinauralSimpleSpatializer::calculateAzimuth()
        float dx = sourceX - listenerX;
        float dz = sourceZ - listenerZ;
        float forwardLen = std::sqrt(forwardX * forwardX + forwardZ * forwardZ);
        if (forwardLen > 0.0001f) { forwardX /= forwardLen; forwardZ /= forwardLen; }
        else                      { forwardX = 0.0f; forwardZ = 1.0f; }

        float azimuth = (std::atan2(dx, dz) - std::atan2(forwardX, forwardZ))
                        * 180.0f / juce::MathConstants<float>::pi;
        while (azimuth >  180.0f) azimuth -= 360.0f;
        while (azimuth < -180.0f) azimuth += 360.0f;

        m_puOwnedHrtfProcessor->preloadIR(azimuth);
    }

}
