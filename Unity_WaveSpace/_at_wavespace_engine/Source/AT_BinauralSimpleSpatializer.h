#pragma once

#include <JuceHeader.h>
#include <memory>
#include "HRTFProcessor.h"

namespace AT
{
    /**
     * @class BinauralSimpleSpatializer
     * @brief Simple binaural panning using direct HRTF convolution on mono source
     * 
     * ARCHITECTURE (same pattern as Spatializer):
     * - processAndAdd() is called ONCE per block:
     *     → Fills internal mono buffer
     *     → Calculates azimuth
     *     → Applies HRTF convolution on ENTIRE block
     *     → Stores result in stereo buffer
     * 
     * - getSample() is called per sample, per channel:
     *     → Simply reads from pre-computed stereo buffer
     * 
     * This matches the existing pattern:
     *   Spatializer: processAndAdd → pushSample to delay line, getSample → read from delay line
     *   BinauralSimpleSpatializer: processAndAdd → HRTF process, getSample → read from stereo buffer
     */
    class BinauralSimpleSpatializer
    {
    public:
        /**
         * @brief Constructor
         */
        BinauralSimpleSpatializer();
        
        /**
         * @brief Destructor
         */
        ~BinauralSimpleSpatializer();
        
        /**
         * @brief Initialize the spatializer with HRTF processor
         * @param hrtfProcessor Shared HRTF processor (same as used for WFS virtualization)
         * @param sampleRate Sample rate in Hz
         * @param samplesPerBlock Maximum samples per block
         * 
         * The HRTF processor is NOT owned by this class - it's shared with the
         * SpatializationEngine's binaural virtualization system.
         */
        void initialize(HRTFProcessor* hrtfProcessor, double sampleRate, int samplesPerBlock);
        
        /**
         * @brief Process an entire audio block and store result
         * 
         * Called ONCE per block from SpatPlayer::processAndAdd().
         * 
         * @param monoInputBuffer Buffer containing mono samples from audio file
         * @param numSamples Number of samples in this block
         * @param sourceX Source X position (Unity coordinates)
         * @param sourceY Source Y position (Unity coordinates)
         * @param sourceZ Source Z position (Unity coordinates)
         * @param listenerX Listener X position (Unity coordinates)
         * @param listenerY Listener Y position (Unity coordinates)
         * @param listenerZ Listener Z position (Unity coordinates)
         * @param listenerForwardX Listener forward vector X component
         * @param listenerForwardY Listener forward vector Y component
         * @param listenerForwardZ Listener forward vector Z component
         * 
         * This method:
         * 1. Copies mono input to internal buffer
         * 2. Calculates azimuth from positions
         * 3. Applies HRTF convolution on entire block
         * 4. Stores stereo result in m_stereoOutputBuffer
         */
        void processBlock(
            const float* monoInputBuffer,
            int numSamples,
            float sourceX, float sourceY, float sourceZ,
            float listenerX, float listenerY, float listenerZ,
            float listenerForwardX, float listenerForwardY, float listenerForwardZ
        );
        
        /**
         * @brief Get a single spatialized sample for a given channel
         * 
         * Called per sample, per channel from SpatPlayer::getSample().
         * Simply reads from the pre-computed stereo buffer.
         * 
         * @param channel Output channel (0 = left, 1 = right)
         * @param sampleIndex Sample index within current block
         * @return Spatialized sample
         * 
         * IMPORTANT: processBlock() must be called first (once per block)
         * before calling getSample() for any sample in that block.
         */
        float getSample(int channel, int sampleIndex) const;
        
        /**
         * @brief Reset internal state (delay lines, etc.)
         */
        void reset();
        
    private:
        /**
         * @brief Calculate azimuth angle from source to listener
         * @param sourceX Source X position
         * @param sourceZ Source Z position
         * @param listenerX Listener X position
         * @param listenerZ Listener Z position
         * @param forwardX Listener forward vector X
         * @param forwardZ Listener forward vector Z
         * @return Azimuth in degrees [-180, 180]
         */
        float calculateAzimuth(
            float sourceX, float sourceZ,
            float listenerX, float listenerZ,
            float forwardX, float forwardZ
        ) const;
        
        // HRTF processor (shared, not owned by this class)
        HRTFProcessor* m_hrtfProcessor;
        
        // Sample rate
        double m_sampleRate;
        
        // Maximum samples per block
        int m_maxSamplesPerBlock;
        
        // Internal buffers for HRTF processing
        // These are sized to m_maxSamplesPerBlock in initialize()
        juce::AudioBuffer<float> m_monoInputBuffer;   // Mono input (1 channel)
        juce::AudioBuffer<float> m_stereoOutputBuffer; // Stereo output (2 channels)
        
        // Number of samples in current block (set by processBlock)
        int m_currentBlockSize;
    };
    
} // namespace AT
