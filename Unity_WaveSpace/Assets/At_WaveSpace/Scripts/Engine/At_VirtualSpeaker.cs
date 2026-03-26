/// @file At_VirtualSpeaker.cs
/// @brief Scene representative of a single WFS loudspeaker.
///
/// @details
/// Drawn in the Scene view as a colored dot (white = active, red = inactive)
/// with a circle showing the maximum delay-compensation radius.

using UnityEngine;
using UnityEngine.SceneManagement;

public class At_VirtualSpeaker : MonoBehaviour
{
    /// <summary>Unique speaker index used by the C++ engine.</summary>
    public int id;

    /// <summary>Current distance to the active player (set by the engine).</summary>
    public float distance;

    /// <summary>False when this speaker is masked out for the current source position.</summary>
    public bool isActive = true;

    /// <summary>
    /// Maximum player-to-speaker distance considered for WFS delay computation.
    /// Set automatically by At_MasterOutput from At_OutputState.maxDistanceForDelay.
    /// </summary>
    public float m_maxDistanceForDelay;

#if UNITY_EDITOR
    private void OnDrawGizmos()
    {
        At_OutputState os = At_AudioEngineUtils.getOutputState(SceneManager.GetActiveScene().name);
        if (os == null) return;

        // Speaker dot: white = active, red = inactive
        Gizmos.color = isActive ? new Color(1f, 1f, 1f, 0.5f) : new Color(1f, 0f, 0f, 0.5f);
        Gizmos.DrawSphere(transform.position, 0.05f);

        // Max-distance circle (WFS delay radius)
        const int CIRCLE_STEPS = 20;
        float angle = 2f * Mathf.PI / CIRCLE_STEPS;
        Gizmos.color = new Color(0f, 0f, 1f, 0.25f);
        for (int i = 0; i < CIRCLE_STEPS; i++)
        {
            Vector3 p0 = transform.position + new Vector3(
                os.maxDistanceForDelay * Mathf.Cos(i * angle), 0f,
                os.maxDistanceForDelay * Mathf.Sin(i * angle));
            Vector3 p1 = transform.position + new Vector3(
                os.maxDistanceForDelay * Mathf.Cos((i + 1) * angle), 0f,
                os.maxDistanceForDelay * Mathf.Sin((i + 1) * angle));
            Gizmos.DrawLine(p0, p1);
        }

        UnityEditor.SceneView.RepaintAll();
    }
#endif
}
