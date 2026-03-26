/// @file WaveSpaceInstaller.cs
/// @brief Automatically enables 'Allow Unsafe Code' and creates required
///        StreamingAssets folders when the package is imported.

using UnityEditor;
using UnityEngine;
using System.IO;

[InitializeOnLoad]
public static class WaveSpaceInstaller
{
    // Runs automatically on every domain reload (import included)
    static WaveSpaceInstaller()
    {
        EnableUnsafeCode();
        EnsureFolderStructure();
    }

    // Also triggered when assets are imported
    static void OnPostprocessAllAssets(
        string[] importedAssets,
        string[] deletedAssets,
        string[] movedAssets,
        string[] movedFromPaths)
    {
        foreach (var path in importedAssets)
        {
            if (path.Contains("At_WaveSpace"))
            {
                EnableUnsafeCode();
                EnsureFolderStructure();
                return;
            }
        }
    }

    static void EnableUnsafeCode()
    {
        // Unity 2019.3+: global PlayerSettings property
        if (!PlayerSettings.allowUnsafeCode)
        {
            PlayerSettings.allowUnsafeCode = true;
            Debug.Log("[WaveSpace] 'Allow unsafe code' enabled automatically.");
        }
    }

    static void EnsureFolderStructure()
    {
        string[] requiredDirs = new string[]
        {
            Application.streamingAssetsPath,
            Path.Combine(Application.streamingAssetsPath, "Audio"),
            Path.Combine(Application.streamingAssetsPath, "HRTF"),
            Path.Combine(Application.streamingAssetsPath, "SpatConfig"),
        };

        bool created = false;
        foreach (string dir in requiredDirs)
        {
            if (!Directory.Exists(dir))
            {
                Directory.CreateDirectory(dir);
                Debug.Log("[WaveSpace] Created folder: " + dir);
                created = true;
            }
        }

        if (created)
            AssetDatabase.Refresh();
    }
}
