/// @file At_Listener.cs
/// @brief Sends the listener transform to the spatialization engine every frame.

using UnityEngine;
using System.Runtime.InteropServices;

public class At_Listener : MonoBehaviour
{
    private const float GIZMO_RADIUS = 0.17f;

    private void Update()
    {
        UpdateTransform();
    }

    /// <summary>
    /// Reads the GameObject transform and forwards position, Euler rotation, and
    /// forward vector to the native library.
    /// </summary>
    private void UpdateTransform()
    {
        float[] position = new float[3];
        float[] rotation = new float[3];
        float[] forward  = new float[3];

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

        position[0] = transform.position.x;
        position[1] = transform.position.y;
        position[2] = transform.position.z;

        rotation[0] = eulerX;
        rotation[1] = eulerY;
        rotation[2] = eulerZ;

        forward[0] = transform.forward.x;
        forward[1] = transform.forward.y;
        forward[2] = transform.forward.z;

        AT_WS_setListenerTransform(position, rotation, forward);
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
