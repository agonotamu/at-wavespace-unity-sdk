/**
 * @file AT_Spatializer.cpp
 * @brief Implementation of the Spatializer class
 * @author Audio Technology Team
 * @date 2025
 */

#pragma once

#include "AT_Spatializer.h"
#include "AT_SpatializationEngine.h"
#include <math.h>
#include "UnityLogger.h"
#include "AT_SpatConfig.h"

#define PI 3.1415926535897932384626433832795

namespace AT
{
    
    Spatializer::Spatializer(SpatPlayer& spatPlayer, int numOutputChannel, int samplesPerBlock, double sampleRate)
        : m_spatPlayer(spatPlayer),
        m_numOutputChannels(numOutputChannel),
        m_samplesPerBlock(samplesPerBlock),
        m_sampleRate(sampleRate),
        m_maxDistance(20.0f),
        m_numActiveSpeakerInMask(0),
        m_isWfsSpeakerMask(true),
        m_pSpatializationEngine(nullptr)
    {
        intializeArray(numOutputChannel);
        intializeDelayLine(sampleRate, samplesPerBlock);
        
        // Initialize LinearSmoothedValue objects with smoothing time
        m_sourcePosXSmoother.reset(sampleRate, SMOOTHING_TIME_SECONDS);
        m_sourcePosZSmoother.reset(sampleRate, SMOOTHING_TIME_SECONDS);
        
        // Set initial values to zero
        m_sourcePosXSmoother.setCurrentAndTargetValue(0.0f);
        m_sourcePosZSmoother.setCurrentAndTargetValue(0.0f);
        
        // Initialise WFS gain smoothers with the correct sample rate.
        // Must be called here so smoothers are ready before the first audio block.
        prepareWfsGainSmoothers(sampleRate, samplesPerBlock);
    }

    Spatializer::~Spatializer()
    {
        // Static arrays: no explicit cleanup needed
    }

    void Spatializer::setSpatializationEngine(SpatializationEngine* engine)
    {
        m_pSpatializationEngine = engine;
    }

    SpatializationEngine* Spatializer::getSpatializationEngine() const
    {
        return m_pSpatializationEngine;
    }

    void Spatializer::intializeDelayLine(int sampleRate, int samplePerBlock)
    {
        juce::dsp::ProcessSpec spec;
        spec.sampleRate = sampleRate;
        spec.maximumBlockSize = juce::uint32(samplePerBlock);
        spec.numChannels = 1;
        m_wfsDelayLine.prepare(spec);

        double numSamples = (m_maxDistance / 340.0f) * sampleRate;
        int maxDelayInSamples = int(std::ceil(numSamples));
        m_wfsDelayLine.setMaximumDelayInSamples(maxDelayInSamples);
        m_wfsDelayLine.reset();
    }

    void Spatializer::intializeArray(int numOutputChannel)
    {
        // Guard: cannot exceed statically allocated maximum
        jassert(numOutputChannel <= MAX_VIRTUAL_SPEAKERS);
        if (numOutputChannel > MAX_VIRTUAL_SPEAKERS)
        {
            numOutputChannel = MAX_VIRTUAL_SPEAKERS;
            DBG("WARNING: Spatializer numOutputChannel clamped to MAX_VIRTUAL_SPEAKERS (" 
                << MAX_VIRTUAL_SPEAKERS << ")");
        }

        // ----------------------------------------------------------------
        // Zero-fill all static arrays with a single memset per array.
        // This replaces ~6155 individual heap allocations that occurred
        // when N=1024 with unique_ptr<unique_ptr<float[]>[]> structures.
        // ----------------------------------------------------------------
        std::memset(m_virtualSpeakerPositions, 0, sizeof(m_virtualSpeakerPositions));
        std::memset(m_virtualSpeakerRotations, 0, sizeof(m_virtualSpeakerRotations));
        std::memset(m_virtualSpeakerForwards,  0, sizeof(m_virtualSpeakerForwards));
        std::memset(m_wfsDelays,               0, sizeof(m_wfsDelays));
        std::memset(m_wfsSpeakerMask,          0, sizeof(m_wfsSpeakerMask));
        std::memset(m_workBufferOutsideMask,   0, sizeof(m_workBufferOutsideMask));

        // Initialize gains to 1.0 for active channels, 0.0 for unused slots
        for (int i = 0; i < numOutputChannel; ++i)
            m_wfsLinGains[i] = 1.0f;
        for (int i = numOutputChannel; i < MAX_VIRTUAL_SPEAKERS; ++i)
            m_wfsLinGains[i] = 0.0f;

        // Initialize speaker mask smoothers
        for (int i = 0; i < MAX_VIRTUAL_SPEAKERS; ++i)
        {
            m_wfsSpeakerMaskSmoother[i].reset(m_sampleRate, SMOOTHING_TIME_SECONDS);
            m_wfsSpeakerMaskSmoother[i].setCurrentAndTargetValue(0.0f);
        }
    }


    void Spatializer::prepareWfsGainSmoothers(double sampleRate, int /*samplesPerBlock*/)
    {
        for (int i = 0; i < MAX_VIRTUAL_SPEAKERS; ++i)
        {
            // 50 ms ramp — avoids amplitude clicks on position updates while
            // staying perceptually tight enough for dynamic scenes.
            m_wfsGainSmoothers[i].reset(sampleRate, SMOOTHING_TIME_SECONDS);
            m_wfsGainSmoothers[i].setCurrentAndTargetValue(1.0f);
        }

        // Smooth the inside-blend factor and the delay reference values with
        // the same ramp so all transitions through the array plane are click-free.
        m_insideBlendSmoother.reset(sampleRate, SMOOTHING_TIME_SECONDS);
        m_insideBlendSmoother.setCurrentAndTargetValue(0.0f);

        m_wfsMaxDelaySmoother.reset(sampleRate, SMOOTHING_TIME_SECONDS);
        m_wfsMaxDelaySmoother.setCurrentAndTargetValue(0.0f);

        m_wfsMinDelaySmoother.reset(sampleRate, SMOOTHING_TIME_SECONDS);
        m_wfsMinDelaySmoother.setCurrentAndTargetValue(0.0f);
    }

    void Spatializer::setIsInsideAndUpdateSpeakerMask()
    {
        
        int negativeDotCount = 0;
        m_numActiveSpeakerInMask = 0;
            
        // ---------------------------------------------------
        // Test if the source is in the listening area
        // ---------------------------------------------------
        // Proposition 2 — raised-cosine taper around the array-plane boundary
        //
        // The inside/outside detection (negativeDotCount) MUST remain based on the
        // raw zero crossing (dotSource < 0) — the taper only affects the mask VALUE
        // in m_workBufferOutsideMask, not the inside/outside classification.
        //
        // Separating the two prevents the taper from breaking focused-source detection:
        // a source at 0.2 m from a 0.3 m-radius array with transW=0.3 would otherwise
        // put transition-band speakers outside negativeDotCount, making the engine
        // classify the source as non-focused → wrong branch → silence.
        // ---------------------------------------------------
        const float eps_mask = m_secondarySourceSize.load(std::memory_order_relaxed);
        const float transW   = juce::jmax(0.05f, eps_mask);

        for (int i = 0; i < m_numOutputChannels; i++)
        {
            // Fix C: use m_sourcePosX/Z (audio-thread-local, advanced by the smoother)
            // instead of m_pSourcePosition[] which is written from the main thread — a data race.
            float SourceSpkVectorX = m_virtualSpeakerPositions[i][0] - m_sourcePosX;
            float SourceSpkVectorZ = m_virtualSpeakerPositions[i][2] - m_sourcePosZ;

            // Dot product of Speaker-Source vector and Speaker-Forward vector (inverted).
            // Virtual mic forward was the opposite of virtual speaker forward, so we negate.
            float dotSource = -(SourceSpkVectorX * (-m_virtualSpeakerForwards[i][0])
                                + SourceSpkVectorZ * (-m_virtualSpeakerForwards[i][2]));

            // ── Inside/outside classification: raw zero crossing (unchanged) ────────
            // Must NOT use the tapered threshold so focused-source detection is correct.
            if (dotSource < 0.0f)
                negativeDotCount++;

            // ── Mask value for the non-focused branch: tapered ───────────────────────
            if (dotSource < -transW)
            {
                m_workBufferOutsideMask[i] = 0.0f;
            }
            else if (dotSource > transW)
            {
                m_workBufferOutsideMask[i] = 1.0f;
                m_numActiveSpeakerInMask++;
            }
            else
            {
                // Transition band: raised-cosine ramp  w = 0.5*(1 − cos(π·t)), t ∈ [0,1]
                const float t = (dotSource + transW) / (2.0f * transW);
                const float w = 0.5f * (1.0f - std::cos(juce::MathConstants<float>::pi * t));
                m_workBufferOutsideMask[i] = w;
                m_numActiveSpeakerInMask++;   // partial contributor
            }
        }
            
        // ---------------------------------------------------
        // Set the speaker mask
        // ---------------------------------------------------
        for (int i = 0; i < m_numOutputChannels; i++)
        {
            // Source is inside the listening area
            if (negativeDotCount == m_numOutputChannels)
            {
                if (i == 0) m_numActiveSpeakerInMask = 0;
                m_isInside = true;
                    
                float SpkSourceVectorX  = m_virtualSpeakerPositions[i][0] - m_sourcePosX; // S − P
                float SpkSourceVectorZ  = m_virtualSpeakerPositions[i][2] - m_sourcePosZ;
                float ListenerSourceVectorX = m_pListenerPosition[0] - m_sourcePosX;
                float ListenerSourceVectorZ = m_pListenerPosition[2] - m_sourcePosZ;
                float dotListener = SpkSourceVectorX * ListenerSourceVectorX
                                    + SpkSourceVectorZ * ListenerSourceVectorZ;
                    
                if (m_isWfsSpeakerMask) {
                    if (dotListener < 0.0f)
                    {
                        m_wfsSpeakerMask[i] = 1.0f;
                        m_numActiveSpeakerInMask++;
                    }
                    else
                    {
                        m_wfsSpeakerMask[i] = 0.0f;
                    }
                }
            }
            // Source is outside the listening area
            else
            {
                m_isInside = false;
                if (m_isWfsSpeakerMask) {
                    m_wfsSpeakerMask[i] = m_workBufferOutsideMask[i];
                }
            }
        }
           
        if (!m_isWfsSpeakerMask) {
            m_numActiveSpeakerInMask = m_numOutputChannels;
            //std::memset(m_wfsSpeakerMask, 1, sizeof(m_wfsSpeakerMask));
            for (int i = 0; i < m_numOutputChannels; i++)
            {
                m_wfsSpeakerMask[i] = 1.0f;
            }
        }

        // ── Compute continuous inside blend factor ────────────────────────────────
        //
        // IMPORTANT: this computation MUST be gated on negativeDotCount == N.
        //
        // For a LINEAR array all forward vectors are parallel, so when the source
        // is outside every speaker has dotSource > 0 → outsideMask = 1 → (1−mask) = 0
        // for all → insideBlend = 0. The unconditional average worked by accident.
        //
        // For a CIRCULAR array the forward vectors point inward.  When the source
        // is OUTSIDE the circle, the speakers on the far side always have dotSource < 0
        // (they face away) regardless → their (1−mask) = 1, which made insideBlend ≈ 0.5
        // even for a fully external source → time-reversal applied in error.
        //
        // Correct logic:
        //   • negativeDotCount < N  → source is outside → insideBlend = 0 (no reversal)
        //   • negativeDotCount == N → source is inside (focused source).
        //       Blend is driven by how close the nearest speaker is to the boundary:
        //       speakers with dotSource ∈ (−transW, 0) contribute a fractional (1−mask),
        //       giving a smooth 0→1 ramp as the source moves inward through the boundary.
        //       The m_insideBlendSmoother then provides per-sample temporal smoothing.
        if (negativeDotCount == m_numOutputChannels)
        {
            float insideSum = 0.0f;
            for (int i = 0; i < m_numOutputChannels; i++)
                insideSum += (1.0f - m_workBufferOutsideMask[i]);
            m_insideBlend = (m_numOutputChannels > 0)
                            ? (insideSum / static_cast<float>(m_numOutputChannels))
                            : 0.0f;
        }
        else
        {
            // Source is outside the array: no time reversal, regardless of geometry.
            m_insideBlend = 0.0f;
        }

    }

    void Spatializer::udpateWfsGainAndDelay()
    {
        // Retrieve smoothed listener position from the SpatializationEngine
        float listenerPosX = 0.0f, listenerPosY = 0.0f, listenerPosZ = 0.0f;
        if (m_pSpatializationEngine != nullptr)
        {
            m_pSpatializationEngine->getSmoothedListenerPosition(listenerPosX, listenerPosY, listenerPosZ);
        }

        m_wfsMinDelay = 0x7f7fffff; // float max value
        m_wfsMaxDelay = 0.0f;

        for (int i = 0; i < m_numOutputChannels; i++)
        {
            float speakerPosX = 0.0f, speakerPosY = 0.0f, speakerPosZ = 0.0f;
            
            if (m_pSpatializationEngine != nullptr)
            {
                m_pSpatializationEngine->getSmoothedVirtualSpeakerPosition(i, speakerPosX, speakerPosY, speakerPosZ);
            }
            else
            {
                // Fallback: use locally stored (non-smoothed) positions
                speakerPosX = m_virtualSpeakerPositions[i][0];
                speakerPosY = m_virtualSpeakerPositions[i][1];
                speakerPosZ = m_virtualSpeakerPositions[i][2];
            }
            
            float directionX = m_sourcePosX - speakerPosX;
            float directionZ = m_sourcePosZ - speakerPosZ;

            // ── Proposition 1 — finite-size source regularisation ──────────────────
            // r_raw : true geometric distance → used for DELAY  (preserves phase).
            // r_amp : regularised distance sqrt(r²+ε²) → used for AMPLITUDE only.
            //         Prevents cos(φ)/sqrt(r) → ∞ as r → 0 (source at array plane).
            //         When m_secondarySourceSize = 0 this degenerates to the original formula.
            // Same ε as the visual shader (_secondarySourceSize) — secondary source radius.
            const float eps_amp = m_secondarySourceSize.load(std::memory_order_relaxed);
            const float r_raw   = std::sqrt(directionX * directionX + directionZ * directionZ);
            const float r_amp   = std::sqrt(r_raw * r_raw + eps_amp * eps_amp);

            float sourceSpeakerDistance = r_raw;   // alias used in the delay path below

            m_wfsDelays[i] = sourceSpeakerDistance / 340.0f;

            // Compute min/max over ACTIVE speakers only (mask > 0.5).
            // Time reversal  d_i = maxD + minD - d_i  assumes that every
            // speaker included in the reference is actually contributing.
            // If an inactive speaker (mask=0) has a larger distance than any
            // active one, maxD becomes too large, the wavefront converges at
            // the wrong point (mirror of the source) and causes L/R inversion.
            // When the mask is disabled all values are 1.0, so this condition
            // is always true and behaviour is identical to the original.
            // When isActiveSpeakersMinMax is enabled, restrict min/max to active speakers
            // (mask > 0.5). When disabled, include all speakers (original behaviour).
            if (!m_isActiveSpeakersMinMax.load() || m_wfsSpeakerMask[i] > 0.5f)
            {
                if (m_wfsDelays[i] > m_wfsMaxDelay)
                    m_wfsMaxDelay = m_wfsDelays[i];
                if (m_wfsDelays[i] < m_wfsMinDelay)
                    m_wfsMinDelay = m_wfsDelays[i];
            }
            
            if (m_isWfsGain.load())
            {
                if (r_raw > 1e-6f)
                {
                    // Normalised direction from source to speaker (2D, X/Z plane).
                    float dirX = -directionX / sourceSpeakerDistance;
                    float dirZ = -directionZ / sourceSpeakerDistance;

                    // cos(φᵢ): dot product of the speaker outward normal with the
                    // normalised source→speaker direction vector.
                    float cosPhi = m_virtualSpeakerForwards[i][0] * dirX
                                 + m_virtualSpeakerForwards[i][2] * dirZ;

                    // External source  (m_isInside = false): active if cosPhi  > 0
                    // Focused  source  (m_isInside = true) : active if cosPhi  < 0  → use -cosPhi
                    // Ref: Berkhout et al., JASA 93(5) 1993
                    //      Caulkins, PhD thesis Univ. Paris 6, 2007 — ch. 2 (focused sources)
                    const float effectiveCos = m_isInside ? -cosPhi : cosPhi;

                    // ── Proposition 3 — soft gate on effectiveCos ────────────────────
                    // Hard gate (effectiveCos > 0 → gain, else 0) produces silence when
                    // the source is exactly at the array plane: the source→speaker
                    // direction is then perpendicular to every speaker normal → cosPhi=0
                    // for all speakers → gain=0.
                    //
                    // Fix: raised-cosine ramp over ±cosFloor, consistent with P2.
                    // cosFloor = ε/r_amp is the half-angle subtended by a secondary source
                    // of radius ε viewed from distance r_amp — the minimum resolvable angle.
                    // When eps_amp = 0, cosFloor = 0 and the hard gate is recovered.
                    const float cosFloor = (r_amp > 1e-6f) ? (eps_amp / r_amp) : 0.0f;

                    float gain;
                    if (effectiveCos > cosFloor)
                    {
                        // Clearly contributing — full formula
                        gain = effectiveCos / std::sqrt(r_amp);
                    }
                    else if (effectiveCos > -cosFloor)
                    {
                        // Transition band: raised-cosine ramp 0 → cosFloor/sqrt(r_amp)
                        // Same window as P2 for consistency.
                        const float tc = (effectiveCos + cosFloor) / (2.0f * cosFloor + 1e-12f);
                        const float wc = 0.5f * (1.0f - std::cos(juce::MathConstants<float>::pi * tc));
                        gain = wc * cosFloor / std::sqrt(r_amp);
                    }
                    else
                    {
                        // Clearly on the wrong side — silent
                        gain = 0.0f;
                    }

                    m_wfsGainSmoothers[i].setTargetValue(gain);
                }
                else
                {
                    // Degenerate case: source sits exactly on this speaker.
                    m_wfsGainSmoothers[i].setTargetValue(0.0f);
                }
            }
            else
            {
                // Delay-only mode: all active speakers drive at unity gain.
                m_wfsGainSmoothers[i].setTargetValue(1.0f);
            }
            
            // Mirror gain target into m_wfsLinGains for the committed snapshot
            // read by the Unity main thread (shader visualisation).
            m_wfsLinGains[i] = m_wfsGainSmoothers[i].getTargetValue();
            
        }

        // ── Set targets for delay-reference smoothers ─────────────────────────────
        // If isActiveSpeakersMinMax left m_wfsMinDelay at float-max (no speaker
        // passed the 0.5 threshold), fall back to the raw geometric min/max so
        // the smoothers always receive a valid target.
        if (m_wfsMinDelay > m_wfsMaxDelay)
        {
            // No qualifying speaker — recompute over all speakers as fallback
            m_wfsMinDelay = 0x7f7fffff;
            m_wfsMaxDelay = 0.0f;
            for (int i = 0; i < m_numOutputChannels; i++)
            {
                if (m_wfsDelays[i] > m_wfsMaxDelay) m_wfsMaxDelay = m_wfsDelays[i];
                if (m_wfsDelays[i] < m_wfsMinDelay) m_wfsMinDelay = m_wfsDelays[i];
            }
        }
        m_wfsMaxDelaySmoother.setTargetValue(m_wfsMaxDelay);
        m_wfsMinDelaySmoother.setTargetValue(m_wfsMinDelay);

        // Advance smoothers by one sample and read current smoothed values
        const float smoothedMaxDelay = m_wfsMaxDelaySmoother.getNextValue();
        const float smoothedMinDelay = m_wfsMinDelaySmoother.getNextValue();

        // Set inside-blend target from the raw estimate computed in
        // setIsInsideAndUpdateSpeakerMask(), then advance by one sample.
        m_insideBlendSmoother.setTargetValue(m_insideBlend);
        const float smoothedBlend = m_insideBlendSmoother.getNextValue();

        // ── Time reversal / blend for focused↔non-focused transition ─────────────
        //
        // Original hard switch:
        //   if (m_isInside) delay[i] = maxDelay + minDelay - delay[i]
        // caused an audible click each time the source crossed the array plane.
        //
        // Replacement: linear interpolation over the transition band defined by
        // secondarySourceSize, using smoothedBlend so the reference values
        // (maxDelay, minDelay) and the blend factor all change gradually.
        //
        //   smoothedBlend = 0  → delay_normal    (source fully outside)
        //   smoothedBlend = 1  → delay_reversed  (source fully inside / focused)
        //   0 < blend < 1      → linear mix within ±secondarySourceSize of plane
        if (smoothedBlend > 0.0f)
        {
            for (int i = 0; i < m_numOutputChannels; i++)
            {
                const float delayNormal   = m_wfsDelays[i];
                const float delayReversed = smoothedMaxDelay + smoothedMinDelay - delayNormal;
                m_wfsDelays[i] = (1.0f - smoothedBlend) * delayNormal
                                +        smoothedBlend  * delayReversed;
            }
        }
        // smoothedBlend == 0: delays unchanged (non-focused), no work needed.
    
        
        // ── atomic commit to the buffer readble by the main thread ──────
       std::memcpy(m_committedWfsDelays,   m_wfsDelays,   m_numOutputChannels * sizeof(float));
       std::memcpy(m_committedWfsLinGains, m_wfsLinGains, m_numOutputChannels * sizeof(float));
       m_outputArraysDirty.store(true, std::memory_order_release);
    }

    void Spatializer::updateSourceParametersTarget()
    {
        m_sourcePosXSmoother.setTargetValue(m_pSourcePosition[0]);
        m_sourcePosZSmoother.setTargetValue(m_pSourcePosition[2]);

        for (int i = 0; i < m_numOutputChannels; i++)
        {
            m_wfsSpeakerMaskSmoother[i].setTargetValue(m_wfsSpeakerMask[i]);
        }
    }

    void Spatializer::advanceSourceSmoothers()
    {
        m_sourcePosX = m_sourcePosXSmoother.getNextValue();
        m_sourcePosZ = m_sourcePosZSmoother.getNextValue();
        
        setIsInsideAndUpdateSpeakerMask();
        udpateWfsGainAndDelay();
    }

    float Spatializer::spatialize(float inputSample, int indexChannel, bool updateReadPointer)
    {
        // Check channel index validity
        if (indexChannel < 0 || indexChannel >= m_numOutputChannels)
            return 0.0f;
        
        // Advance global smoothers once per sample (first channel only)
        if (indexChannel == 0 && m_pSpatializationEngine != nullptr)
        {
            m_pSpatializationEngine->advanceGlobalSmoothers();
        }
        
        // Get the smoothed speaker mask value for this channel
        float smoothedMask = m_wfsSpeakerMaskSmoother[indexChannel].getNextValue();
        
        // Skip WFS calculation if this channel is masked (silent)
        const float maskThreshold = 0.001f;
        if (smoothedMask < maskThreshold)
        {
            if (updateReadPointer)
                m_wfsDelayLine.popSample(0, 0.0f, true);
            return 0.0f;
        }
        
        // Get the delay for this output channel (in seconds)
        float delayInSeconds = m_wfsDelays[indexChannel];
        float delayInSamples = delayInSeconds * static_cast<float>(m_sampleRate);
        
        // Clamp delay to valid range
        float maxDelayInSamples = (m_maxDistance / 340.0f) * static_cast<float>(m_sampleRate);
        if (delayInSamples < 0.0f)
            delayInSamples = 0.0f;
        else if (delayInSamples > maxDelayInSamples)
            delayInSamples = maxDelayInSamples;
        
        float delayedSample = m_wfsDelayLine.popSample(0, delayInSamples, updateReadPointer);
        
        // Advance the WFS gain smoother by one sample and apply it.
        // getNextValue() returns the next point on the ramp, ensuring
        // click-free gain transitions when positions change.
        const float smoothedGain = m_wfsGainSmoothers[indexChannel].getNextValue();

        
        float outputSample = 0.0f;
        if (m_numActiveSpeakerInMask != 0)
        {
            outputSample = delayedSample
                         * smoothedGain
                         * smoothedMask
                         / std::sqrt((float)m_numActiveSpeakerInMask);
        }
        
        return outputSample;
    }

    // ============================================================================
    // GLOBAL SPATIALIZATION ENGINE SETTINGS
    // ============================================================================

    void Spatializer::setListenerTransform(float* position, float* rotation, float* forward)
    {
        if (position != nullptr)
            for (int i = 0; i < 3; i++) m_pListenerPosition[i] = position[i];
        if (rotation != nullptr)
            for (int i = 0; i < 3; i++) m_pListenerRotation[i] = rotation[i];
        if (forward != nullptr)
            for (int i = 0; i < 3; i++) m_pListenerForward[i] = forward[i];
    }

    void Spatializer::setVirtualSpeakerTransform(float* positions, float* rotations, float* forwards, int virtualSpeakerCount)
    {
        if (virtualSpeakerCount != m_numOutputChannels)
            return;

        if (positions != nullptr)
        {
            for (int i = 0; i < m_numOutputChannels; i++)
                for (int j = 0; j < 3; j++)
                    m_virtualSpeakerPositions[i][j] = positions[i * 3 + j];
        }
        if (rotations != nullptr)
        {
            for (int i = 0; i < m_numOutputChannels; i++)
                for (int j = 0; j < 3; j++)
                    m_virtualSpeakerRotations[i][j] = rotations[i * 3 + j];
        }
        if (forwards != nullptr)
        {
            for (int i = 0; i < m_numOutputChannels; i++)
                for (int j = 0; j < 3; j++)
                    m_virtualSpeakerForwards[i][j] = forwards[i * 3 + j];
        }
    }

    void Spatializer::setMaxDistanceForDelay(float maxDistance)
    {
        m_maxDistance = maxDistance;
    }

    // ============================================================================
    // SPATIALIZATION ENGINE SETTINGS FOR PLAYERS
    // ============================================================================

    void Spatializer::setPlayerTransform(float* position, float* rotation, float* forward)
    {
        if (position != nullptr)
            for (int i = 0; i < 3; i++) m_pSourcePosition[i] = position[i];
        if (rotation != nullptr)
            for (int i = 0; i < 3; i++) m_pSourceRotation[i] = rotation[i];
        if (forward != nullptr)
            for (int i = 0; i < 3; i++) m_pSourceForward[i] = forward[i];
    }

    void Spatializer::setPlayerAttenuation(float attenuation)
    {
        m_attenuation = attenuation;
    }

    void Spatializer::setPlayerMinDistance(float minDistance)
    {
        m_minDistance = minDistance;
    }

    void Spatializer::enablePlayerSpeakerMask(bool isWfsSpeakerMask)
    {
        m_isWfsSpeakerMask = isWfsSpeakerMask;
    }

    void Spatializer::setIsWfsGain(bool isWfsGain)
    {
        m_isWfsGain.store(isWfsGain);

        // When disabling WFS gain, ramp all smoothers back to unity so the
        // transition is click-free even if udpateWfsGainAndDelay() is not
        // called immediately after.
        if (!isWfsGain)
        {
            for (int i = 0; i < MAX_VIRTUAL_SPEAKERS; ++i)
                m_wfsGainSmoothers[i].setTargetValue(1.0f);
        }
    }

    // ============================================================================
    // SPATIALIZATION ENGINE GETTERS FOR PLAYERS
    // ============================================================================

    void Spatializer::getPlayerWfsDelay(float* delay, int arraySize)
    {
        /*
        if (delay != nullptr && arraySize == m_numOutputChannels)
        {
            for (int i = 0; i < m_numOutputChannels; i++)
                delay[i] = m_wfsDelays[i];
        }
        */
        // Read from the coherent snapshot (never partially written)
        int count = std::min(arraySize, m_numOutputChannels);
        std::memcpy(delay, m_committedWfsDelays, count * sizeof(float));
        m_outputArraysDirty.store(false, std::memory_order_release);
    }

    void Spatializer::getWfsLinGain(float* linGain, int arraySize)
    {
        /*
        if (linGain != nullptr && arraySize == m_numOutputChannels)
        {
            for (int i = 0; i < m_numOutputChannels; i++)
                linGain[i] = m_wfsLinGains[i];
        }
        */
        int count = std::min(arraySize, m_numOutputChannels);
        std::memcpy(linGain, m_committedWfsLinGains, count * sizeof(float));
    }

    void Spatializer::getPlayerSpeakerMask(float* speakerMask, int arraySize)
    {
        if (speakerMask != nullptr && arraySize == m_numOutputChannels)
        {
            for (int i = 0; i < m_numOutputChannels; i++)
                speakerMask[i] = m_wfsSpeakerMask[i];
        }
    }

    // ============================================================================
    // GETTERS FOR SIMPLE BINAURAL MODE
    // ============================================================================

    void Spatializer::getSourcePosition(float& x, float& y, float& z) const
    {
        x = m_pSourcePosition[0];
        y = m_pSourcePosition[1];
        z = m_pSourcePosition[2];
    }

    void Spatializer::getListenerPosition(float& x, float& y, float& z) const
    {
        x = m_pListenerPosition[0];
        y = m_pListenerPosition[1];
        z = m_pListenerPosition[2];
    }

    void Spatializer::getListenerForward(float& x, float& y, float& z) const
    {
        x = m_pListenerForward[0];
        y = m_pListenerForward[1];
        z = m_pListenerForward[2];
    }

    float Spatializer::getNumActiveSpeakerInMask()
    {
        return m_numActiveSpeakerInMask;
    }

    float Spatializer::computeDistanceGain() const
    {
        // No attenuation requested — fast path
        if (m_attenuation <= 0.0f)
            return 1.0f;

        // Listener position — prefer smoothed values from the engine so the gain
        // follows the same position ramp as all other spatial parameters.
        float lx = m_pListenerPosition[0];
        float ly = m_pListenerPosition[1];
        float lz = m_pListenerPosition[2];

        if (m_pSpatializationEngine != nullptr)
        {
            float dummy;
            m_pSpatializationEngine->getSmoothedListenerPosition(lx, dummy, lz);
            ly = dummy;
        }

        // 3D Euclidean distance from source to listener
        const float dx = m_pSourcePosition[0] - lx;
        const float dy = m_pSourcePosition[1] - ly;
        const float dz = m_pSourcePosition[2] - lz;
        const float d  = std::sqrt(dx * dx + dy * dy + dz * dz);

        // Clamp to minimum distance — no boost below minDistance
        const float dClamped = juce::jmax(d, m_minDistance);

        // 1 / d^attenuation, expressed as exp(-attenuation * log(d))
        const float gain = (dClamped > 0.0f)
                           ? std::pow(m_minDistance / dClamped, m_attenuation)
                           : 1.0f;

        // Clamp to [0, 1]: never boost, never go negative
        return juce::jlimit(0.0f, 1.0f, gain);
    }

} // namespace AT
