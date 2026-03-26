#include "AT_BinauralSimpleSpatializer.h"
#include "UnityLogger.h"
#include <cmath>
#include "AT_SpatConfig.h"

namespace AT
{
    BinauralSimpleSpatializer::BinauralSimpleSpatializer()
        : m_hrtfProcessor(nullptr)
        , m_sampleRate(48000.0)
        , m_maxSamplesPerBlock(0)
        , m_monoInputBuffer(1, 1)    // Will be resized in initialize()
        , m_stereoOutputBuffer(2, 1)  // Will be resized in initialize()
        , m_currentBlockSize(0)
    {
    }

    BinauralSimpleSpatializer::~BinauralSimpleSpatializer()
    {
        // HRTF processor is not owned by this class — no cleanup needed
    }

    void BinauralSimpleSpatializer::initialize(HRTFProcessor* hrtfProcessor, double sampleRate, int samplesPerBlock)
    {
        m_hrtfProcessor = hrtfProcessor;
        m_sampleRate = sampleRate;
        m_maxSamplesPerBlock = samplesPerBlock;

        // Allocate buffers sized to the maximum block size.
        // Reused for each block — no reallocation on the audio thread.
        m_monoInputBuffer.setSize(1, m_maxSamplesPerBlock, false, false, true);
        m_stereoOutputBuffer.setSize(2, m_maxSamplesPerBlock, false, false, true);

        reset();
    }

    void BinauralSimpleSpatializer::reset()
    {
        m_monoInputBuffer.clear();
        m_stereoOutputBuffer.clear();
        m_currentBlockSize = 0;
    }

    void BinauralSimpleSpatializer::processBlock(
        const float* monoInputBuffer,
        int numSamples,
        float sourceX, float sourceY, float sourceZ,
        float listenerX, float listenerY, float listenerZ,
        float listenerForwardX, float listenerForwardY, float listenerForwardZ
    )
    {
        m_currentBlockSize = numSamples;

        if (!m_hrtfProcessor)
        {
            m_stereoOutputBuffer.clear(0, numSamples);
            return;
        }

        if (!m_hrtfProcessor->isHRTFLoaded())
        {
            LOG_WARNING("Simple binaural mode active but no HRTF loaded. Call AT_SPAT_loadDefaultHRTF() first.");
            m_stereoOutputBuffer.clear(0, numSamples);
            return;
        }

        if (numSamples > m_maxSamplesPerBlock)
        {
            LOG_ERROR("Block size " << numSamples << " exceeds maximum " << m_maxSamplesPerBlock);
            m_stereoOutputBuffer.clear(0, numSamples);
            return;
        }

        juce::FloatVectorOperations::copy(
            m_monoInputBuffer.getWritePointer(0),
            monoInputBuffer,
            numSamples
        );

        float azimuth = calculateAzimuth(
            sourceX, sourceZ,
            listenerX, listenerZ,
            listenerForwardX, listenerForwardZ
        );

        m_stereoOutputBuffer.clear(0, numSamples);

        m_hrtfProcessor->processAndAccumulate(
            m_stereoOutputBuffer,   // Output: stereo (2 channels)
            m_monoInputBuffer,      // Input: mono (1 channel)
            0,                      // Source channel index
            numSamples,             // Number of samples to process
            azimuth,                // Azimuth angle in degrees
            0.0f                    // Elevation (unused)
        );
    }

    float BinauralSimpleSpatializer::getSample(int channel, int sampleIndex) const
    {
        if (channel < 0 || channel >= 2)
            return 0.0f;

        if (sampleIndex < 0 || sampleIndex >= m_currentBlockSize)
            return 0.0f;

        return m_stereoOutputBuffer.getSample(channel, sampleIndex);
    }

    float BinauralSimpleSpatializer::calculateAzimuth(
        float sourceX, float sourceZ,
        float listenerX, float listenerZ,
        float forwardX, float forwardZ
    ) const
    {
        // Vector from listener to source (XZ plane)
        float dx = sourceX - listenerX;
        float dz = sourceZ - listenerZ;

        // Normalize forward vector
        float forwardLen = std::sqrt(forwardX * forwardX + forwardZ * forwardZ);

        if (forwardLen < 0.0001f)
        {
            forwardX = 0.0f;
            forwardZ = 1.0f;
        }
        else
        {
            forwardX /= forwardLen;
            forwardZ /= forwardLen;
        }

        // Unity left-handed coordinate system (viewed from above, Y up):
        //        Z+ (Forward)
        //         |
        // X- <---+---> X+
        //  (L)   |   (R)
        //        Z-

        float angleToSource = std::atan2(dx, dz);
        float forwardAngle  = std::atan2(forwardX, forwardZ);
        float azimuthRad    = angleToSource - forwardAngle;
        float azimuthDeg    = azimuthRad * 180.0f / juce::MathConstants<float>::pi;

        // Normalize to [-180, 180]
        while (azimuthDeg >  180.0f) azimuthDeg -= 360.0f;
        while (azimuthDeg < -180.0f) azimuthDeg += 360.0f;

        return azimuthDeg;
    }

} // namespace AT
