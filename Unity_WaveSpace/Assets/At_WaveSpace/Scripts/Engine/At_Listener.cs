/// @file At_Listener.cs
/// @brief Sends the listener transform to the spatialization engine every frame.
///
/// @details
/// The native engine's startup geometry gate holds audio output until the first
/// listener AND speaker transforms have been adopted, so this component must run
/// (and send) from the very first frame of play mode. Speaker transforms are sent
/// at the end of At_MasterOutput.Awake(); this script's first Update() completes
/// the pair within one frame — well inside the native gate timeout.

using UnityEngine;
using System.Runtime.InteropServices;

public class At_Listener : MonoBehaviour
{
    private const float GIZMO_RADIUS = 0.17f;

    // Pre-allocated transform arrays — avoids per-frame heap allocation
    // (same pattern and rationale as At_MasterOutput's speaker arrays).
    private readonly float[] m_position = new float[3];
    private readonly float[] m_rotation = new float[3];
    private readonly float[] m_forward  = new float[3];

    // Cached reference used to skip native calls when the engine is not running
    // (e.g. failed device setup). Resolved lazily because At_MasterOutput.Awake()
    // ordering relative to this component is not guaranteed.
    private At_MasterOutput m_masterOutput;

    private void Update()
    {
        if (m_masterOutput == null)
        {
            m_masterOutput = FindObjectOfType<At_MasterOutput>();
            if (m_masterOutput == null) return;   // no engine in scene
        }

        // Do not hammer the native library when the engine failed to start or
        // has been shut down (scene unload). All other AT_WS_* callers in the
        // package guard the same way.
        if (!m_masterOutput.isInitialized) return;

        UpdateTransform();
    }

    /// <summary>
    /// Reads the GameObject transform and forwards position, Euler rotation, and
    /// forward vector to the native library.
    /// </summary>
    private void UpdateTransform()
    {
        float eulerX = transform.eulerAngles.x;
        float eulerY = transform.eulerAngles.y;
        float eulerZ = transform.eulerAngles.z;

        // Normalize gimbal-lock edge case (Y=180, Z=180)
        if (eulerY == 180 && eulerZ == 180)
        {
            eulerX = 180 - eulerX;
            eulerY = 0;
            eulerZ = 0;
        }

        Vector3 pos = transform.position;
        Vector3 fwd = transform.forward;

        m_position[0] = pos.x;  m_position[1] = pos.y;  m_position[2] = pos.z;
        m_rotation[0] = eulerX; m_rotation[1] = eulerY; m_rotation[2] = eulerZ;
        m_forward[0]  = fwd.x;  m_forward[1]  = fwd.y;  m_forward[2]  = fwd.z;

        AT_WS_setListenerTransform(m_position, m_rotation, m_forward);
    }

#if UNITY_EDITOR
    private void OnDrawGizmos()
    {
        Gizmos.color = Color.white;
        Gizmos.DrawSphere(transform.position, GIZMO_RADIUS);
        UnityEditor.SceneView.RepaintAll();
    }
#endif

    #region DLL Imports
    [DllImport("at_wavespace_engine", CallingConvention = CallingConvention.StdCall)]
    private static extern void AT_WS_setListenerTransform(float[] position, float[] rotation, float[] forward);
    #endregion
}
