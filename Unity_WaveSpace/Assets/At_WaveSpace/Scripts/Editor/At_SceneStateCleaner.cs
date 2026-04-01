/// @file At_SceneStateCleaner.cs
/// @brief Deletes the AT WaveSpace state file when its associated scene is deleted.
///
/// @details
/// Unity does not clean up StreamingAssets files when a scene is removed from the
/// project.  If a new scene is later created with the same name, the stale state
/// file would be loaded by At_AudioEngineUtils on startup, producing incorrect
/// device, channel, and player configuration.
///
/// This AssetModificationProcessor intercepts scene deletions at Edit time and
/// removes the matching _States.state file from StreamingAssets before Unity
/// completes the asset removal.
///
/// Placement: must live in an Editor folder (e.g. Assets/At_WaveSpace/Scripts/Editor/).

using UnityEditor;
using System.IO;

public class At_SceneStateCleaner : AssetModificationProcessor
{
    /// <summary>
    /// Called by Unity just before any asset is deleted from the project.
    /// When the asset is a scene (.unity), removes the corresponding
    /// AT WaveSpace _States.state file and clears the in-memory state cache.
    /// </summary>
    /// <returns>
    /// Always returns <see cref="AssetDeleteResult.DidNotDelete"/> so that Unity
    /// continues with its own scene deletion.
    /// </returns>
    static AssetDeleteResult OnWillDeleteAsset(string assetPath, RemoveAssetOptions options)
    {
        if (!assetPath.EndsWith(".unity", System.StringComparison.OrdinalIgnoreCase))
            return AssetDeleteResult.DidNotDelete;

        string sceneName = Path.GetFileNameWithoutExtension(assetPath);

        // Remove the in-memory state so the stale data is not reused within
        // the same Editor session if a new scene with the same name is created.
        At_AudioEngineUtils.RemoveSceneState(sceneName);

        // Delete the _States.state file from StreamingAssets
        string stateFilePath = At_AudioEngineUtils.GetFilePathForStates(sceneName + "_States.state");
        if (File.Exists(stateFilePath))
        {
            try
            {
                File.Delete(stateFilePath);

                // Remove the Unity .meta sidecar if it exists
                string metaPath = stateFilePath + ".meta";
                if (File.Exists(metaPath))
                    File.Delete(metaPath);

                // Defer the AssetDatabase refresh to after Unity has finished
                // its own deletion pass.  Calling Refresh() directly inside
                // OnWillDeleteAsset while the scene is still open triggers a
                // warning from Unity's asset pipeline.
                EditorApplication.delayCall += AssetDatabase.Refresh;
            }
            catch (System.Exception e)
            {
                UnityEngine.Debug.LogError(
                    $"[AT_WS] Failed to delete state file for scene '{sceneName}': {e.Message}\n" +
                    $"Path: {stateFilePath}");
            }
        }

        // Let Unity proceed with deleting the scene asset itself.
        return AssetDeleteResult.DidNotDelete;
    }
}
