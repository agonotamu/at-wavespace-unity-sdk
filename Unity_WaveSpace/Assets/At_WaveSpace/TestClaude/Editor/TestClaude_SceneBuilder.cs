/// @file TestClaude_SceneBuilder.cs
/// @brief Construit la scène de test Assets/At_WaveSpace/TestClaude/TestClaude_Orbit.unity.
///
/// @details
/// Part d'une copie de Assets/Scenes/debug.unity (At_MasterOutput, At_Listener, rig de 32
/// haut-parleurs virtuels), retire ses At_Player et ajoute un TestClaude_OrbitSpawner.
/// La configuration de sortie est reprise de la scène debug, avec un buffer de 512.
///
/// Le fichier d'état est écrit dans StreamingAssets (TestClaude_Orbit_States.state) : c'est
/// le seul fichier hors de At_WaveSpace/TestClaude, parce qu'At_AudioEngineUtils ne lit les
/// états que là. Il est supprimé automatiquement avec la scène (At_SceneStateCleaner).
///
/// Menu : TestClaude > Créer ou recréer la scène de test (orbite).

using UnityEditor;
using UnityEditor.SceneManagement;
using UnityEngine;

public static class TestClaude_SceneBuilder
{
    private const string BASE_SCENE   = "Assets/Scenes/debug.unity";
    private const string BASE_NAME    = "debug";
    private const string TARGET_SCENE = "Assets/At_WaveSpace/TestClaude/TestClaude_Orbit.unity";
    private const string TARGET_NAME  = "TestClaude_Orbit";

    private const int BUFFER_SIZE          = 512;
    private const int BUFFER_SIZE_POPUP_ID = 3;   // 64, 128, 256, 512, …

    [MenuItem("TestClaude/Créer ou recréer la scène de test (orbite)")]
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
        outputState.bufferSize         = BUFFER_SIZE;
        outputState.selectedBufferSize = BUFFER_SIZE_POPUP_ID;

        // --- Scène : copie, nettoyage, ajout du spawner ------------------------------------
        if (AssetDatabase.LoadAssetAtPath<SceneAsset>(TARGET_SCENE) != null)
            AssetDatabase.DeleteAsset(TARGET_SCENE);   // supprime aussi l'ancien fichier d'état

        if (!AssetDatabase.CopyAsset(BASE_SCENE, TARGET_SCENE))
        {
            Debug.LogError("[TestClaude] Copie de la scène impossible vers " + TARGET_SCENE);
            return false;
        }

        var scene = EditorSceneManager.OpenScene(TARGET_SCENE, OpenSceneMode.Single);

        // Les sources sont créées à l'exécution par le spawner : on retire les joueurs copiés.
        foreach (At_Player p in Object.FindObjectsOfType<At_Player>(true))
            Object.DestroyImmediate(p.gameObject);

        if (Object.FindObjectOfType<At_MasterOutput>(true) == null || Object.FindObjectOfType<At_Listener>(true) == null)
        {
            Debug.LogError("[TestClaude] La scène de base ne contient pas At_MasterOutput et At_Listener.");
            return false;
        }

        // At_MasterOutput.Awake() ne relit pas binauralRenderMode depuis l'état : c'est la valeur
        // sérialisée sur le composant qui est envoyée au moteur. On l'aligne sur l'état.
        At_MasterOutput master = Object.FindObjectOfType<At_MasterOutput>(true);
        master.binauralRenderMode = outputState.binauralRenderMode;
        EditorUtility.SetDirty(master);

        var spawnerObject = new GameObject("TestClaude_OrbitSpawner");
        spawnerObject.AddComponent<TestClaude_OrbitSpawner>();

        // --- État de la scène, par le mécanisme du projet ----------------------------------
        At_AudioEngineUtils.audioEngineStatesDictionary[TARGET_NAME] =
            new At_3DAudioEngineState { outputState = outputState };
        At_AudioEngineUtils.SaveAllState(TARGET_NAME);

        EditorSceneManager.MarkSceneDirty(scene);
        if (!EditorSceneManager.SaveScene(scene))
        {
            Debug.LogError("[TestClaude] Enregistrement de la scène impossible.");
            return false;
        }

        AssetDatabase.Refresh();
        Debug.Log($"[TestClaude] Scène créée : {TARGET_SCENE} (périphérique « {outputState.audioDeviceName} », " +
                  $"{outputState.numVirtualSpeakers} HP virtuels, buffer {outputState.bufferSize}).");
        return true;
    }
}
