#include "HRTFProcessor.h"
#include "AT_SpatConfig.h"

HRTFProcessor::HRTFProcessor()
    : currentSampleRate(44100.0)
    , maxBlockSize(512)
    , targetDistance(1.0f)
    // leftConvolver and rightConvolver are value members — default-constructed,
    // no heap allocation needed here. Previously two make_unique<Convolution>()
    // calls were placed here, each causing a heap allocation.
{
    currentAzimuth.store(0.0f);
    currentElevation.store(0.0f);
    currentDistance.store(1.0f);
    needsUpdate.store(true);
    
    // NOTE: leftConvolver and rightConvolver are now direct members,
    // initialised by their default constructors above — no code needed here.
}

HRTFProcessor::~HRTFProcessor()
{
    // Reset convolution state before destruction to cleanly release FFT resources.
    // With value members, the objects will be destroyed automatically afterward.
    leftConvolver.reset();
    rightConvolver.reset();
}

void HRTFProcessor::prepare(double sampleRate, int samplesPerBlock)
{
    currentSampleRate = sampleRate;
    maxBlockSize = samplesPerBlock;
    
    juce::dsp::ProcessSpec spec;
    spec.sampleRate       = sampleRate;
    spec.maximumBlockSize = static_cast<juce::uint32>(samplesPerBlock);
    spec.numChannels      = 1;
    
    // Value members — access with . instead of ->
    leftConvolver.prepare(spec);
    rightConvolver.prepare(spec);
    
    // Pre-allocate buffers to maximum size (no allocations in audio thread)
    monoBuffer.setSize(1, samplesPerBlock, false, true, false);
    tempLeftBuffer.setSize(1, samplesPerBlock, false, true, false);
    tempRightBuffer.setSize(1, samplesPerBlock, false, true, false);
    
    monoBuffer.clear();
    tempLeftBuffer.clear();
    tempRightBuffer.clear();
    
    if (!sofaReader.isLoaded())
    {
        sofaReader.createDefaultHRTF();
        updateIRs();
    }
}

void HRTFProcessor::reset()
{
    leftConvolver.reset();
    rightConvolver.reset();
    
    monoBuffer.clear();
    tempLeftBuffer.clear();
    tempRightBuffer.clear();
}

bool HRTFProcessor::loadHRTF(const juce::File& sofaFile)
{
    bool success = sofaReader.loadFile(sofaFile);
    
    if (success)
    {
        needsUpdate.store(true);
        updateIRs();
    }
    
    return success;
}

bool HRTFProcessor::loadHRTFFromFile(const std::string& filePath)
{
    juce::File hrtfFile(filePath);
    return loadHRTF(hrtfFile);
}

bool HRTFProcessor::loadDefaultHRTF()
{
    sofaReader.createDefaultHRTF();
    needsUpdate.store(true);
    updateIRs();
    return true;
}

void HRTFProcessor::loadHRTFFromSharedReader(const SOFAReader& sharedReader)
{
    // In-memory copy — no file I/O, no FFT re-init.
    // SOFAReader contains only standard vectors and scalars,
    // so the compiler-generated copy assignment is correct and complete.
    sofaReader = sharedReader;
    needsUpdate.store(true);
    updateIRs();
}

void HRTFProcessor::updateIRs()
{
    if (!sofaReader.isLoaded())
        return;

    float azimuth   = currentAzimuth.load();
    float elevation = currentElevation.load();
    float distance  = currentDistance.load();

    // Fix E layer 1: index guard — reload only when the nearest SOFA measurement
    // index actually changes. Between grid points the same IR is optimal.
    int newIndex = sofaReader.hasDistanceInfo()
                    ? sofaReader.getNearestPositionIndexWithDistance(azimuth, elevation, distance)
                    : sofaReader.getNearestPositionIndex(azimuth, elevation);

    m_pendingIRIndex = newIndex;

    // Fix E layer 2: cooldown — after each loadImpulseResponse(), block further
    // reloads for IR_RELOAD_COOLDOWN_BLOCKS blocks so the JUCE crossfade never piles up.
    if (m_irReloadCooldown > 0)
    {
        --m_irReloadCooldown;
        if (m_irReloadCooldown == 0 && m_pendingIRIndex != m_lastIRIndex)
            ; // fall through to load below
        else
        {
            needsUpdate.store(false);
            return;
        }
    }

    if (m_pendingIRIndex == m_lastIRIndex)
    {
        needsUpdate.store(false);
        return;
    }

    std::vector<float> leftIR, rightIR;
    bool success;
    if (sofaReader.hasDistanceInfo())
        success = sofaReader.getIRsForPositionWithDistance(azimuth, elevation, distance, leftIR, rightIR);
    else
        success = sofaReader.getIRsForPosition(azimuth, elevation, leftIR, rightIR);

    if (success)
    {
        int irLength = static_cast<int>(leftIR.size());

        double hrtfSampleRate = static_cast<double>(sofaReader.getSampleRate());
        bool needsResampling  = std::abs(hrtfSampleRate - currentSampleRate) > 0.1;

        if (needsResampling)
        {
            double ratio   = currentSampleRate / hrtfSampleRate;
            int newLength  = static_cast<int>(std::ceil(irLength * ratio));
            std::vector<float> resampledLeft(newLength);
            std::vector<float> resampledRight(newLength);
            resampleIR(leftIR,  resampledLeft,  ratio);
            resampleIR(rightIR, resampledRight, ratio);
            leftIR   = resampledLeft;
            rightIR  = resampledRight;
            irLength = newLength;
        }

        // Truncate IR to m_maxIRLength if a limit is set (0 = no limit).
        if (m_maxIRLength > 0 && irLength > m_maxIRLength)
            irLength = m_maxIRLength;

        juce::AudioBuffer<float> tempLeftIR(1, irLength);
        juce::AudioBuffer<float> tempRightIR(1, irLength);
        juce::FloatVectorOperations::copy(tempLeftIR.getWritePointer(0),  leftIR.data(),  irLength);
        juce::FloatVectorOperations::copy(tempRightIR.getWritePointer(0), rightIR.data(), irLength);

        leftConvolver.loadImpulseResponse(std::move(tempLeftIR),
                                          currentSampleRate,
                                          juce::dsp::Convolution::Stereo::no,
                                          juce::dsp::Convolution::Trim::no,
                                          juce::dsp::Convolution::Normalise::no);

        rightConvolver.loadImpulseResponse(std::move(tempRightIR),
                                           currentSampleRate,
                                           juce::dsp::Convolution::Stereo::no,
                                           juce::dsp::Convolution::Trim::no,
                                           juce::dsp::Convolution::Normalise::no);

        m_lastIRIndex      = m_pendingIRIndex;
        m_irReloadCooldown = IR_RELOAD_COOLDOWN_BLOCKS;
        needsUpdate.store(false);
    }
}

void HRTFProcessor::resampleIR(const std::vector<float>& input, std::vector<float>& output, double ratio)
{
    int inputLength  = static_cast<int>(input.size());
    int outputLength = static_cast<int>(output.size());
    
    for (int i = 0; i < outputLength; ++i)
    {
        double srcPos  = static_cast<double>(i) / ratio;
        int srcIndex   = static_cast<int>(srcPos);
        double frac    = srcPos - srcIndex;
        
        if (srcIndex < inputLength - 1)
            output[i] = static_cast<float>((1.0 - frac) * input[srcIndex] + frac * input[srcIndex + 1]);
        else if (srcIndex < inputLength)
            output[i] = input[srcIndex];
        else
            output[i] = 0.0f;
    }
}

void HRTFProcessor::process(juce::AudioBuffer<float>& buffer, float azimuth, float elevation)
{
    const int numSamples  = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();
    
    if (numSamples == 0 || !sofaReader.isLoaded())
        return;
    
    float currentAz = currentAzimuth.load();
    float currentEl = currentElevation.load();
    
    if (std::abs(azimuth - currentAz) > 0.5f || std::abs(elevation - currentEl) > 0.5f)
    {
        currentAzimuth.store(azimuth);
        currentElevation.store(elevation);
        updateIRs();
    }
    
    monoBuffer.clear();
    
    if (numChannels >= 1)
        juce::FloatVectorOperations::copy(monoBuffer.getWritePointer(0),
                                          buffer.getReadPointer(0), numSamples);
    
    tempLeftBuffer.clear();
    tempRightBuffer.clear();
    juce::FloatVectorOperations::copy(tempLeftBuffer.getWritePointer(0),
                                      monoBuffer.getReadPointer(0), numSamples);
    juce::FloatVectorOperations::copy(tempRightBuffer.getWritePointer(0),
                                      monoBuffer.getReadPointer(0), numSamples);
    
    juce::dsp::AudioBlock<float> leftBlock(tempLeftBuffer.getArrayOfWritePointers(),   1, numSamples);
    juce::dsp::AudioBlock<float> rightBlock(tempRightBuffer.getArrayOfWritePointers(), 1, numSamples);
    juce::dsp::ProcessContextReplacing<float> leftContext(leftBlock);
    juce::dsp::ProcessContextReplacing<float> rightContext(rightBlock);
    
    leftConvolver.process(leftContext);
    rightConvolver.process(rightContext);
    
    buffer.clear();
    
    if (numChannels >= 2)
    {
        juce::FloatVectorOperations::copy(buffer.getWritePointer(0),
                                          tempLeftBuffer.getReadPointer(0),  numSamples);
        juce::FloatVectorOperations::copy(buffer.getWritePointer(1),
                                          tempRightBuffer.getReadPointer(0), numSamples);
    }
    else if (numChannels == 1)
    {
        juce::FloatVectorOperations::copy(buffer.getWritePointer(0),
                                          tempLeftBuffer.getReadPointer(0), numSamples);
        juce::FloatVectorOperations::multiply(buffer.getWritePointer(0), 0.5f, numSamples);
        juce::FloatVectorOperations::addWithMultiply(buffer.getWritePointer(0),
                                                     tempRightBuffer.getReadPointer(0),
                                                     0.5f, numSamples);
    }
}

void HRTFProcessor::process(juce::AudioBuffer<float>& buffer, float azimuth, float elevation, float distance)
{
    const int numSamples  = buffer.getNumSamples();
    const int numChannels = buffer.getNumChannels();
    
    if (numSamples == 0 || !sofaReader.isLoaded())
        return;
    
    float currentAz   = currentAzimuth.load();
    float currentEl   = currentElevation.load();
    float currentDist = currentDistance.load();
    
    bool needsNewIRs = (std::abs(azimuth   - currentAz)   > 0.5f) ||
                       (std::abs(elevation - currentEl)   > 0.5f) ||
                       (sofaReader.hasDistanceInfo() && std::abs(distance - currentDist) > 0.05f);
    
    if (needsNewIRs)
    {
        currentAzimuth.store(azimuth);
        currentElevation.store(elevation);
        currentDistance.store(distance);
        updateIRs();
    }
    
    monoBuffer.clear();
    
    if (numChannels >= 1)
        juce::FloatVectorOperations::copy(monoBuffer.getWritePointer(0),
                                          buffer.getReadPointer(0), numSamples);
    
    tempLeftBuffer.clear();
    tempRightBuffer.clear();
    juce::FloatVectorOperations::copy(tempLeftBuffer.getWritePointer(0),
                                      monoBuffer.getReadPointer(0), numSamples);
    juce::FloatVectorOperations::copy(tempRightBuffer.getWritePointer(0),
                                      monoBuffer.getReadPointer(0), numSamples);
    
    juce::dsp::AudioBlock<float> leftBlock(tempLeftBuffer.getArrayOfWritePointers(),   1, numSamples);
    juce::dsp::AudioBlock<float> rightBlock(tempRightBuffer.getArrayOfWritePointers(), 1, numSamples);
    juce::dsp::ProcessContextReplacing<float> leftContext(leftBlock);
    juce::dsp::ProcessContextReplacing<float> rightContext(rightBlock);
    
    leftConvolver.process(leftContext);
    rightConvolver.process(rightContext);
    
    buffer.clear();
    
    if (numChannels >= 2)
    {
        juce::FloatVectorOperations::copy(buffer.getWritePointer(0),
                                          tempLeftBuffer.getReadPointer(0),  numSamples);
        juce::FloatVectorOperations::copy(buffer.getWritePointer(1),
                                          tempRightBuffer.getReadPointer(0), numSamples);
    }
    else if (numChannels == 1)
    {
        juce::FloatVectorOperations::copy(buffer.getWritePointer(0),
                                          tempLeftBuffer.getReadPointer(0), numSamples);
        juce::FloatVectorOperations::multiply(buffer.getWritePointer(0), 0.5f, numSamples);
        juce::FloatVectorOperations::addWithMultiply(buffer.getWritePointer(0),
                                                     tempRightBuffer.getReadPointer(0),
                                                     0.5f, numSamples);
    }
}   

void HRTFProcessor::processAndAccumulate(juce::AudioBuffer<float>& outputBuffer,
                                         const juce::AudioBuffer<float>& sourceBuffer,
                                         int sourceChannel, int numSamples,
                                         float azimuth, float elevation)
{
    if (numSamples == 0 || !sofaReader.isLoaded())
        return;
    
    const int numSourceChannels = sourceBuffer.getNumChannels();
    const int numOutputChannels = outputBuffer.getNumChannels();
    
    if (sourceChannel >= numSourceChannels || numOutputChannels < 2)
        return;
    
    // Always update and call updateIRs(). The old 0.5° threshold is incompatible
    // with the azimuth smoother in SpatializationEngine: per-block deltas are ~0.03°
    // so the threshold would never fire. updateIRs() is guarded by m_lastIRIndex
    // and only calls loadImpulseResponse() when the nearest SOFA bin changes.
    currentAzimuth.store(azimuth);
    currentElevation.store(elevation);
    updateIRs();
    
    juce::FloatVectorOperations::copy(monoBuffer.getWritePointer(0),
                                      sourceBuffer.getReadPointer(sourceChannel), numSamples);
    
    juce::FloatVectorOperations::copy(tempLeftBuffer.getWritePointer(0),
                                      monoBuffer.getReadPointer(0), numSamples);
    juce::FloatVectorOperations::copy(tempRightBuffer.getWritePointer(0),
                                      monoBuffer.getReadPointer(0), numSamples);
    
    juce::dsp::AudioBlock<float> leftBlock(tempLeftBuffer.getArrayOfWritePointers(),   1, numSamples);
    juce::dsp::AudioBlock<float> rightBlock(tempRightBuffer.getArrayOfWritePointers(), 1, numSamples);
    juce::dsp::ProcessContextReplacing<float> leftContext(leftBlock);
    juce::dsp::ProcessContextReplacing<float> rightContext(rightBlock);
    
    leftConvolver.process(leftContext);
    rightConvolver.process(rightContext);
    
    juce::FloatVectorOperations::add(outputBuffer.getWritePointer(0),
                                     tempLeftBuffer.getReadPointer(0),  numSamples);
    juce::FloatVectorOperations::add(outputBuffer.getWritePointer(1),
                                     tempRightBuffer.getReadPointer(0), numSamples);
}

void HRTFProcessor::processAndAccumulate(juce::AudioBuffer<float>& outputBuffer,
                                         const juce::AudioBuffer<float>& sourceBuffer,
                                         int sourceChannel, int numSamples,
                                         float azimuth, float elevation, float distance)
{
    if (numSamples == 0 || !sofaReader.isLoaded())
        return;
    
    const int numSourceChannels = sourceBuffer.getNumChannels();
    const int numOutputChannels = outputBuffer.getNumChannels();
    
    if (sourceChannel >= numSourceChannels || numOutputChannels < 2)
        return;
    
    // Same reasoning as the no-distance overload: remove the threshold.
    currentAzimuth.store(azimuth);
    currentElevation.store(elevation);
    currentDistance.store(distance);
    updateIRs();
    
    juce::FloatVectorOperations::copy(monoBuffer.getWritePointer(0),
                                      sourceBuffer.getReadPointer(sourceChannel), numSamples);
    
    juce::FloatVectorOperations::copy(tempLeftBuffer.getWritePointer(0),
                                      monoBuffer.getReadPointer(0), numSamples);
    juce::FloatVectorOperations::copy(tempRightBuffer.getWritePointer(0),
                                      monoBuffer.getReadPointer(0), numSamples);
    
    juce::dsp::AudioBlock<float> leftBlock(tempLeftBuffer.getArrayOfWritePointers(),   1, numSamples);
    juce::dsp::AudioBlock<float> rightBlock(tempRightBuffer.getArrayOfWritePointers(), 1, numSamples);
    juce::dsp::ProcessContextReplacing<float> leftContext(leftBlock);
    juce::dsp::ProcessContextReplacing<float> rightContext(rightBlock);
    
    leftConvolver.process(leftContext);
    rightConvolver.process(rightContext);
    
    juce::FloatVectorOperations::add(outputBuffer.getWritePointer(0),
                                     tempLeftBuffer.getReadPointer(0),  numSamples);
    juce::FloatVectorOperations::add(outputBuffer.getWritePointer(1),
                                     tempRightBuffer.getReadPointer(0), numSamples);
}

void HRTFProcessor::preloadIR(float azimuth, float elevation)
{
    currentAzimuth.store(azimuth);
    currentElevation.store(elevation);
    if (sofaReader.isLoaded())
        updateIRs();  // triggers loadImpulseResponse() asynchronously
}
