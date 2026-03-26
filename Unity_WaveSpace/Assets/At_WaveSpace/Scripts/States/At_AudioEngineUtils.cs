/// @file At_AudioEngineUtils.cs
/// @brief Static utility class for managing, saving, and loading engine state.
///
/// @details
/// Maintains a per-scene dictionary of At_3DAudioEngineState objects that hold
/// output configuration, player states, and virtual speaker layout.
/// All persistent data is serialized to JSON files in StreamingAssets.
/// State is loaded once on first access (static constructor) and saved explicitly.

using System;
using System.Collections.Generic;
using System.IO;
using UnityEngine;
using UnityEngine.SceneManagement;

public class At_AudioEngineUtils : MonoBehaviour
{
    /// <summary>Per-scene engine state dictionary (key = scene name).</summary>
    static public Dictionary<string, At_3DAudioEngineState> audioEngineStatesDictionary
        = new Dictionary<string, At_3DAudioEngineState>();

    // Internal reference kept for legacy compatibility.
    static public At_3DAudioEngineState audioEngineStates;

    // -------------------------------------------------------------------------
    // Static constructor — loads all state files on first access
    // -------------------------------------------------------------------------

    static At_AudioEngineUtils()
    {
        Debug.Log("[AudioEngineUtils] Loading state files...");
        LoadAll();
        Debug.Log("[AudioEngineUtils] State files loaded.");
    }

    // -------------------------------------------------------------------------
    // Player state management
    // -------------------------------------------------------------------------

    /// <summary>Renames a player entry in the state and saves.</summary>
    public static void changePlayerName(string sceneName, string previousName, string newName)
    {
        foreach (At_PlayerState ps in audioEngineStatesDictionary[sceneName].playerStates)
        {
            if (ps.name == previousName)
                ps.name = newName;
        }
        SaveAllState(sceneName);
    }

    /// <summary>Removes a player state by GUID and saves.</summary>
    public static void removePlayerWithGuid(string sceneName, string guid)
    {
        List<At_PlayerState> states = audioEngineStatesDictionary[sceneName].playerStates;
        for (int i = 0; i < states.Count; i++)
        {
            if (states[i].guid == guid)
            {
                states.RemoveAt(i);
                break;
            }
        }
        SaveAllState(sceneName);
    }

    /// <summary>
    /// Removes state entries for players that no longer exist in the scene.
    /// </summary>
    public static void CleanAllStates(string sceneName)
    {
        At_3DAudioEngineState engineState = audioEngineStatesDictionary[sceneName];
        At_Player[] scenePlayers = Resources.FindObjectsOfTypeAll(typeof(At_Player)) as At_Player[];

        bool clean = false;
        while (!clean)
        {
            for (int i = 0; i < engineState.playerStates.Count; i++)
            {
                if (!findGuidInScene(engineState.playerStates[i].guid, scenePlayers))
                {
                    removePlayerWithGuid(sceneName, engineState.playerStates[i].guid);
                    break;
                }
            }

            clean = true;
            foreach (At_PlayerState ps in engineState.playerStates)
            {
                if (!findGuidInScene(ps.guid, scenePlayers)) { clean = false; break; }
            }
        }
    }

    /// <summary>
    /// Creates a new player state entry for the given scene, GUID, and name.
    /// </summary>
    public static At_PlayerState createNewPlayerStateWithGuidAndName(string sceneName, string guid, string name)
    {
        At_3DAudioEngineState state = audioEngineStatesDictionary[sceneName]
            ?? new At_3DAudioEngineState();

        At_PlayerState ps = new At_PlayerState { name = name, guid = guid };
        state.playerStates.Add(ps);
        return state.playerStates[state.playerStates.Count - 1];
    }

    /// <summary>Returns the player state matching the given GUID, or null.</summary>
    public static At_PlayerState getPlayerStateWithGuidAndName(string sceneName, string guid, string name)
    {
        if (!audioEngineStatesDictionary.ContainsKey(sceneName)) return null;
        return audioEngineStatesDictionary[sceneName]?.getPlayerState(guid);
    }

    // -------------------------------------------------------------------------
    // Output state
    // -------------------------------------------------------------------------

    /// <summary>
    /// Returns the output state for the given scene, loading it from disk if needed.
    /// </summary>
    public static At_OutputState getOutputState(string sceneName)
    {
        if (!audioEngineStatesDictionary.ContainsKey(sceneName))
        {
            audioEngineStatesDictionary[sceneName] = new At_3DAudioEngineState();
        }

        At_3DAudioEngineState engineState = audioEngineStatesDictionary[sceneName];

        if (engineState.outputState == null)
        {
            string json      = ReadFromFile(sceneName + "_States.state");
            string firstLine = json.Split('\n')[0];
            At_OutputState os = new At_OutputState();
            JsonUtility.FromJsonOverwrite(firstLine, os);
            engineState.outputState = os ?? new At_OutputState();
        }

        return engineState.outputState;
    }

    // -------------------------------------------------------------------------
    // Serialization
    // -------------------------------------------------------------------------

    /// <summary>
    /// Serializes the output state and all player states for the given scene to a
    /// single newline-delimited JSON file in StreamingAssets.
    /// </summary>
    public static void SaveAllState(string sceneName)
    {
        At_3DAudioEngineState engineState = audioEngineStatesDictionary[sceneName];

        string json = JsonUtility.ToJson(engineState.outputState);
        foreach (At_PlayerState ps in engineState.playerStates)
            json += "\n" + JsonUtility.ToJson(ps);

        WriteToFile(sceneName + "_States.state", json);
    }

    /// <summary>Returns the full file system path for a state file name.</summary>
    public static string GetFilePathForStates(string fileName)
    {
        return Application.streamingAssetsPath + "/" + fileName;
    }

    // -------------------------------------------------------------------------
    // Scene object lookup helpers
    // -------------------------------------------------------------------------

    /// <summary>Returns the GameObject carrying an At_Player with the given GUID.</summary>
    public static GameObject getGameObjectWithGuid(string guid)
    {
        GameObject[] all = Resources.FindObjectsOfTypeAll(typeof(GameObject)) as GameObject[];
        foreach (GameObject go in all)
        {
            At_Player p = go.GetComponent<At_Player>();
            if (p != null && p.guid == guid) return go;
        }
        return null;
    }

    /// <summary>Returns the At_Player with the given GUID.</summary>
    public static At_Player getAtPlayerWithGuid(string guid)
    {
        At_Player[] players = Resources.FindObjectsOfTypeAll(typeof(At_Player)) as At_Player[];
        foreach (At_Player p in players)
            if (p.guid == guid) return p;
        return null;
    }

    // -------------------------------------------------------------------------
    // Private helpers
    // -------------------------------------------------------------------------

    private static bool findGuidInScene(string guid, At_Player[] players)
    {
        foreach (At_Player p in players)
            if (p.guid == guid) return true;
        return false;
    }

    private static void LoadAll()
    {
        string statesPath = GetFilePathForStates("");
        if (!System.IO.Directory.Exists(statesPath)) return;

        foreach (string filePath in System.IO.Directory.GetFiles(statesPath))
        {
            string fileName = filePath.Replace(statesPath, "");

            if (fileName.Contains("_States") && System.IO.Path.GetExtension(fileName) != ".meta")
            {
                string sceneName = fileName.Replace("_States.state", "");
                audioEngineStates = new At_3DAudioEngineState();

                if (!audioEngineStatesDictionary.ContainsKey(sceneName))
                    audioEngineStatesDictionary.Add(sceneName, audioEngineStates);

                string   json  = ReadFromFile(fileName);
                string[] lines = json.Split('\n');

                for (int i = 0; i < lines.Length; i++)
                {
                    if (i == 0)
                    {
                        At_OutputState os = new At_OutputState();
                        JsonUtility.FromJsonOverwrite(lines[i], os);
                        audioEngineStatesDictionary[sceneName].outputState = os;
                    }
                    else if (lines[i].Contains("\"type\":0"))
                    {
                        At_PlayerState ps = new At_PlayerState();
                        JsonUtility.FromJsonOverwrite(lines[i], ps);
                        audioEngineStatesDictionary[sceneName].playerStates.Add(ps);
                    }
                }
            }
        }
    }

    private static void WriteToFile(string fileName, string json)
    {
        string path = GetFilePathForStates(fileName);
        using (StreamWriter writer = new StreamWriter(new FileStream(path, FileMode.Create)))
            writer.Write(json);
    }

    private static string ReadFromFile(string fileName)
    {
        string path = GetFilePathForStates(fileName);
        if (!File.Exists(path))
        {
            Debug.LogWarning("[AudioEngineUtils] State file not found: " + path);
            return "";
        }
        using (StreamReader reader = new StreamReader(path))
            return reader.ReadToEnd();
    }
}
