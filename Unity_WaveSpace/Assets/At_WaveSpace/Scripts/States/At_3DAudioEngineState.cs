/// @file At_3DAudioEngineState.cs
/// @brief In-memory representation of the full engine state for one scene.
///
/// @details
/// Holds the output configuration, the list of player states, and the virtual
/// speaker layout for a given scene. Serialized to/from JSON by At_AudioEngineUtils.

using System.Collections.Generic;
using UnityEngine;

public class At_3DAudioEngineState
{
    // -------------------------------------------------------------------------
    // Output state
    // -------------------------------------------------------------------------

    /// <summary>Output bus configuration (device, channels, gain, HRTF, …).</summary>
    public At_OutputState outputState = null;

    public At_OutputState getOutputState()  => outputState;
    public void setOutputState(At_OutputState state) { outputState = state; }

    // -------------------------------------------------------------------------
    // Player states
    // -------------------------------------------------------------------------

    /// <summary>Persistent state for every At_Player in the scene.</summary>
    public List<At_PlayerState> playerStates = new List<At_PlayerState>();

    /// <summary>Returns the player state matching the given GUID, or null.</summary>
    public At_PlayerState getPlayerState(string guid)
    {
        for (int i = 0; i < playerStates.Count; i++)
        {
            if (playerStates[i].guid == guid)
                return playerStates[i];
        }
        return null;
    }

}
