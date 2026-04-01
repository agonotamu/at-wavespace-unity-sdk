using System.Collections;
using System.Collections.Generic;
using UnityEngine;
using UnityEditor;

public class At_SpeakerConfig : MonoBehaviour
{
    static float virtualSpeakerScale = 0.6f;

    // =========================================================================
    // PUBLIC ENTRY POINT
    // =========================================================================

    /// <summary>
    /// Generates a speaker configuration in the scene.
    /// Only VirtualSpeaker objects are created — VirtualMic no longer exists.
    /// </summary>
    /// <param name="speakers">Output array filled with the created speaker GameObjects</param>
    /// <param name="virtualSpeakerRigSize">Size of the speaker rig in meters</param>
    /// <param name="outputChannelCount">Number of speakers to create</param>
    /// <param name="outputConfigDimension">1=Linear, 2=Square, 3=Square 3/4, 4=Circle, 5=Circle 1/2</param>
    /// <param name="virtualSpkParent">Parent GameObject for all created speakers</param>
    static public void addSpeakerConfigToScene(
        ref GameObject[] speakers,
        float            virtualSpeakerRigSize,
        int              outputChannelCount,
        int              outputConfigDimension,
        GameObject       virtualSpkParent)
    {
        if      (outputConfigDimension == 1) linearConfig       (ref speakers, virtualSpeakerRigSize, outputChannelCount, virtualSpkParent);
        else if (outputConfigDimension == 2) squareConfig       (ref speakers, virtualSpeakerRigSize, outputChannelCount, virtualSpkParent);
        else if (outputConfigDimension == 3) squareHalfConfig   (ref speakers, virtualSpeakerRigSize, outputChannelCount, virtualSpkParent);
        else if (outputConfigDimension == 4) circleConfig       (ref speakers, virtualSpeakerRigSize, outputChannelCount, virtualSpkParent);
        else if (outputConfigDimension == 5) circleHalfConfig_12(ref speakers, virtualSpeakerRigSize, outputChannelCount, virtualSpkParent);
    }

    // =========================================================================
    // PRIVATE HELPER — create and register a single VirtualSpeaker
    // =========================================================================

    static GameObject createSpeaker(int index, Vector3 position, Vector3 eulerAngles, GameObject parent)
    {
        GameObject spk = new GameObject("spk" + index);
        spk.AddComponent<At_VirtualSpeaker>();
        spk.transform.position    = position;
        spk.transform.localScale  = new Vector3(virtualSpeakerScale, virtualSpeakerScale, virtualSpeakerScale);
        spk.transform.eulerAngles = eulerAngles;
        spk.GetComponent<At_VirtualSpeaker>().id       = index;
        spk.GetComponent<At_VirtualSpeaker>().distance = 0;
        spk.transform.SetParent(parent.transform);
#if UNITY_EDITOR
        PrefabUtility.RecordPrefabInstancePropertyModifications(spk.transform);
#endif
        return spk;
    }

    // =========================================================================
    // 1D — LINEAR
    // Speakers are placed along X and face inward (-Z = toward audience).
    // =========================================================================
    static void linearConfig(
        ref GameObject[] speakers,
        float virtualSpeakerRigSize,
        int   outputChannelCount,
        GameObject virtualSpkParent)
    {
        float step = (outputChannelCount > 1) ? virtualSpeakerRigSize / (float)(outputChannelCount - 1) : 0f;
        speakers = new GameObject[outputChannelCount];
        Vector3 origin = virtualSpkParent.transform.parent.transform.position;

        for (int i = 0; i < outputChannelCount; i++)
        {
            Vector3 pos = origin + new Vector3(-virtualSpeakerRigSize / 2f + i * step, 0f, 0f);
            // Face inward toward audience (-Z): eulerAngles Y = 180
            speakers[i] = createSpeaker(i, pos, new Vector3(0f, 180f, 0f), virtualSpkParent);
        }
    }

    // =========================================================================
    // 2D — SQUARE (full perimeter)
    // The original code placed mics with spkAngle facing outward, then rotated
    // speakers 180° so they face inward. We apply the inward angle directly.
    // =========================================================================
    static void squareConfig(
        ref GameObject[] speakers,
        float virtualSpeakerRigSize,
        int   outputChannelCount,
        GameObject virtualSpkParent)
    {
        speakers = new GameObject[outputChannelCount];

        int   numPerSide      = outputChannelCount / 4;
        float interDistance   = virtualSpeakerRigSize / numPerSide;
        float distStep        = interDistance / 2f;
        Vector3 position      = Vector3.zero;
        float   spkAngle      = 0f;
        Vector3 origin        = virtualSpkParent.transform.parent.transform.position;

        for (int i = 0; i < outputChannelCount; i++)
        {
            if (i < outputChannelCount / 4)
            {
                position.x = -virtualSpeakerRigSize / 2f + distStep;
                position.z =  virtualSpeakerRigSize / 2f;
                spkAngle   = 180f;   // faces -Z (inward from front wall)
            }
            else if (i < outputChannelCount / 2)
            {
                position.x =  virtualSpeakerRigSize / 2f;
                position.z =  virtualSpeakerRigSize / 2f - (distStep - (outputChannelCount / 4) * interDistance);
                spkAngle   = 270f;   // faces -X (inward from right wall)
            }
            else if (i < outputChannelCount * 3 / 4)
            {
                position.x =  virtualSpeakerRigSize / 2f - (distStep - (outputChannelCount / 2) * interDistance);
                position.z = -virtualSpeakerRigSize / 2f;
                spkAngle   = 0f;     // faces +Z (inward from back wall)
            }
            else
            {
                position.x = -virtualSpeakerRigSize / 2f;
                position.z = -virtualSpeakerRigSize / 2f + (distStep - (outputChannelCount * 3 / 4) * interDistance);
                spkAngle   = 90f;    // faces +X (inward from left wall)
            }
            distStep += interDistance;

            speakers[i] = createSpeaker(i, origin + position, new Vector3(0f, spkAngle, 0f), virtualSpkParent);
        }
    }

    // =========================================================================
    // 2D — HALF-SQUARE (3 sides: left, front, right — back side open)
    // Speakers are distributed as evenly as possible across the three sides.
    // For n speakers: side[i] gets n/3 + (1 if i < n%3 else 0) speakers.
    // Speakers on each side are spaced with a half-step inset from the corners,
    // matching the convention used by squareConfig.
    // =========================================================================
    static void squareHalfConfig(
        ref GameObject[] speakers,
        float virtualSpeakerRigSize,
        int   outputChannelCount,
        GameObject virtualSpkParent)
    {
        speakers = new GameObject[outputChannelCount];
        Vector3 origin = virtualSpkParent.transform.parent.transform.position;

        // Distribute outputChannelCount speakers across 3 sides as evenly as possible.
        int n0 = outputChannelCount / 3 + (outputChannelCount % 3 > 0 ? 1 : 0); // left
        int n1 = outputChannelCount / 3 + (outputChannelCount % 3 > 1 ? 1 : 0); // front
        int n2 = outputChannelCount - n0 - n1;                                   // right

        int spkIdx = 0;

        // ── Left side (X = -rigSize/2), speakers face right (+X, Y angle = 90°) ──
        if (n0 > 0)
        {
            float step = virtualSpeakerRigSize / n0;
            for (int i = 0; i < n0; i++, spkIdx++)
            {
                Vector3 pos = origin + new Vector3(
                    -virtualSpeakerRigSize / 2f,
                     0f,
                    -virtualSpeakerRigSize / 2f + step * 0.5f + i * step);
                speakers[spkIdx] = createSpeaker(spkIdx, pos, new Vector3(0f, 90f, 0f), virtualSpkParent);
            }
        }

        // ── Front side (Z = +rigSize/2), speakers face inward (-Z, Y angle = 180°) ─
        if (n1 > 0)
        {
            float step = virtualSpeakerRigSize / n1;
            for (int i = 0; i < n1; i++, spkIdx++)
            {
                Vector3 pos = origin + new Vector3(
                    -virtualSpeakerRigSize / 2f + step * 0.5f + i * step,
                     0f,
                     virtualSpeakerRigSize / 2f);
                speakers[spkIdx] = createSpeaker(spkIdx, pos, new Vector3(0f, 180f, 0f), virtualSpkParent);
            }
        }

        // ── Right side (X = +rigSize/2), speakers face left (-X, Y angle = 270°) ──
        if (n2 > 0)
        {
            float step = virtualSpeakerRigSize / n2;
            for (int i = 0; i < n2; i++, spkIdx++)
            {
                Vector3 pos = origin + new Vector3(
                     virtualSpeakerRigSize / 2f,
                     0f,
                     virtualSpeakerRigSize / 2f - step * 0.5f - i * step);
                speakers[spkIdx] = createSpeaker(spkIdx, pos, new Vector3(0f, 270f, 0f), virtualSpkParent);
            }
        }
    }

    // =========================================================================
    // 2D — CIRCLE (full)
    // Speakers face inward (LookAt center), preserving original behaviour.
    // =========================================================================
    static void circleConfig(
        ref GameObject[] speakers,
        float virtualSpeakerRigSize,
        int   outputChannelCount,
        GameObject virtualSpkParent)
    {
        speakers = new GameObject[outputChannelCount];
        float angularStep = 2f * Mathf.PI / outputChannelCount;
        float angle       = -Mathf.PI / 2f;
        Vector3 center    = virtualSpkParent.transform.parent.transform.position;

        for (int i = 0; i < outputChannelCount; i++)
        {
            Vector3 pos = center + new Vector3(
                (virtualSpeakerRigSize / 2f) * Mathf.Sin(angle), 0f,
                (virtualSpeakerRigSize / 2f) * Mathf.Cos(angle));

            GameObject spk = new GameObject("spk" + i);
            spk.AddComponent<At_VirtualSpeaker>();
            spk.transform.position   = pos;
            spk.transform.localScale = new Vector3(virtualSpeakerScale, virtualSpeakerScale, virtualSpeakerScale);
            spk.transform.LookAt(center);   // faces inward (toward center = audience)
            spk.GetComponent<At_VirtualSpeaker>().id       = i;
            spk.GetComponent<At_VirtualSpeaker>().distance = 0;
            spk.transform.SetParent(virtualSpkParent.transform);
#if UNITY_EDITOR
            PrefabUtility.RecordPrefabInstancePropertyModifications(spk.transform);
#endif
            speakers[i] = spk;
            angle += angularStep;
        }
    }

    // =========================================================================
    // 2D — HALF-CIRCLE
    // =========================================================================
    static void circleHalfConfig_12(
        ref GameObject[] speakers,
        float virtualSpeakerRigSize,
        int   outputChannelCount,
        GameObject virtualSpkParent)
    {
        speakers = new GameObject[outputChannelCount];
        float angularStep = Mathf.PI / (float)(outputChannelCount - 1);
        float angle       = -Mathf.PI / 2f;
        Vector3 center    = virtualSpkParent.transform.parent.transform.position;

        for (int i = 0; i < outputChannelCount; i++)
        {
            Vector3 pos = center + new Vector3(
                (virtualSpeakerRigSize / 2f) * Mathf.Sin(angle), 0f,
                (virtualSpeakerRigSize / 2f) * Mathf.Cos(angle));

            GameObject spk = new GameObject("spk" + i);
            spk.AddComponent<At_VirtualSpeaker>();
            spk.transform.position   = pos;
            spk.transform.localScale = new Vector3(virtualSpeakerScale, virtualSpeakerScale, virtualSpeakerScale);
            spk.transform.LookAt(center);   // faces inward
            spk.GetComponent<At_VirtualSpeaker>().id       = i;
            spk.GetComponent<At_VirtualSpeaker>().distance = 0;
            spk.transform.SetParent(virtualSpkParent.transform);
#if UNITY_EDITOR
            PrefabUtility.RecordPrefabInstancePropertyModifications(spk.transform);
#endif
            speakers[i] = spk;
            angle += angularStep;
        }
    }
}
