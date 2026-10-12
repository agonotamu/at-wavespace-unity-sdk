/// @file TestClaude_StaticSceneBuilder.cs
/// @brief Construit Assets/At_WaveSpace/TestClaude/TestClaude_Static100.unity : 100 At_Player
///        statiques, désactivés, à activer un par un pendant la lecture.
///
/// @details
/// Part de TestClaude_Orbit.unity (même rig, même auditeur, même configuration de sortie),
/// retire le spawner dynamique et crée les joueurs dans la scène. Chaque joueur a son entrée
/// dans le fichier d'état (StreamingAssets/TestClaude_Static100_States.state), comme un
/// joueur créé à la main dans l'éditeur.
///
/// Compile avec la version actuelle du moteur et avec Unity_WaveSpace_oldVersion.
///
/// Menu : TestClaude > Créer ou recréer la scène de test (100 joueurs statiques).

using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;

public static class TestClaude_StaticSceneBuilder
{
    private const string BASE_SCENE   = "Assets/At_WaveSpace/TestClaude/TestClaude_Orbit.unity";
    private const string BASE_NAME    = "TestClaude_Orbit";
    private const string TARGET_SCENE = "Assets/At_WaveSpace/TestClaude/TestClaude_Static100.unity";
    private const string TARGET_NAME  = "TestClaude_Static100";

    private const int    PLAYER_COUNT = 100;
    private const string AUDIO_FILE   = "Audio/Speech_sample.wav";   // relatif à StreamingAssets

    [MenuItem("TestClaude/Créer ou recréer la scène de test (100 joueurs statiques)")]
    public static void BuildFromMenu()
    {
        if (!EditorSceneManager.SaveCurrentModifiedScenesIfUserWantsTo()) return;
        if (Build())
            EditorUtility.DisplayDialog("TestClaude", "Scène créée :\n" + TARGET_SCENE, "OK");
    }

    /// <summary>Point d'entrée pour Unity en mode batch (-executeMethod).</summary>
    public static void BuildBatch()
    {
        if (!Build())
            EditorApplication.Exit(1);
    }

    private static bool Build()
    {
        if (AssetDatabase.LoadAssetAtPath<SceneAsset>(BASE_SCENE) == null)
        {
            Debug.LogError("[TestClaude] Scène de base introuvable : " + BASE_SCENE);
            return false;
        }

        // --- Configuration de sortie : copie de celle de la scène de base ------------------
        At_OutputState baseState = At_AudioEngineUtils.getOutputState(BASE_NAME);
        var outputState = new At_OutputState();
        JsonUtility.FromJsonOverwrite(JsonUtility.ToJson(baseState), outputState);

        At_AudioEngineUtils.audioEngineStatesDictionary[TARGET_NAME] =
            new At_3DAudioEngineState { outputState = outputState };

        // --- Scène ------------------------------------------------------------------------
        if (AssetDatabase.LoadAssetAtPath<SceneAsset>(TARGET_SCENE) != null)
            AssetDatabase.DeleteAsset(TARGET_SCENE);

        if (!AssetDatabase.CopyAsset(BASE_SCENE, TARGET_SCENE))
        {
            Debug.LogError("[TestClaude] Copie de la scène impossible vers " + TARGET_SCENE);
            return false;
        }

        var scene = EditorSceneManager.OpenScene(TARGET_SCENE, OpenSceneMode.Single);

        foreach (TestClaude_OrbitSpawner spawner in Object.FindObjectsOfType<TestClaude_OrbitSpawner>(true))
            Object.DestroyImmediate(spawner.gameObject);
        foreach (At_Player p in Object.FindObjectsOfType<At_Player>(true))
            Object.DestroyImmediate(p.gameObject);

        var root = new GameObject("TestClaude_Players");
        root.AddComponent<TestClaude_StaticOrbit>();

        for (int i = 0; i < PLAYER_COUNT; i++)
        {
            string name = $"TestClaude_Player_{i:000}";
            var go = new GameObject(name);
            go.transform.SetParent(root.transform, false);

            // Champs lus par addPlayer() AVANT le chargement de l'état : ils doivent être
            // justes sur le composant lui-même. playbackSpeed n'est jamais relu depuis l'état.
            var p = go.AddComponent<At_Player>();
            p.guid             = System.Guid.NewGuid().ToString();
            p.is3D             = true;
            p.isLooping        = true;
            p.isPlayingOnAwake = true;
            p.playbackSpeed    = 1f;
            p.attenuation      = 0f;
            p.minDistance      = 0.1f;

            At_PlayerState ps = At_AudioEngineUtils.createNewPlayerStateWithGuidAndName(TARGET_NAME, p.guid, name);
            ps.fileName               = AUDIO_FILE;
            ps.gain                   = 0f;
            ps.is3D                   = true;
            ps.isPlayingOnAwake       = true;
            ps.isLooping              = true;
            ps.playbackSpeed          = 1f;
            ps.attenuation            = 0f;      // pas d'atténuation de distance
            ps.minDistance            = 0.1f;
            ps.numChannelsInAudiofile = 1;

            GameObject marker = GameObject.CreatePrimitive(PrimitiveType.Sphere);
            Object.DestroyImmediate(marker.GetComponent<Collider>());
            marker.name = "marker";
            marker.transform.SetParent(go.transform, false);
            marker.transform.localScale = Vector3.one * 0.3f;

            go.SetActive(false);   // désactivé par défaut : à activer pendant la lecture
        }

        At_AudioEngineUtils.SaveAllState(TARGET_NAME);

        EditorSceneManager.MarkSceneDirty(scene);
        if (!EditorSceneManager.SaveScene(scene))
        {
            Debug.LogError("[TestClaude] Enregistrement de la scène impossible.");
            return false;
        }

        AssetDatabase.Refresh();
        Debug.Log($"[TestClaude] Scène créée : {TARGET_SCENE} ({PLAYER_COUNT} joueurs désactivés, " +
                  $"périphérique « {outputState.audioDeviceName} », buffer {outputState.bufferSize}).");
        return true;
    }
}
