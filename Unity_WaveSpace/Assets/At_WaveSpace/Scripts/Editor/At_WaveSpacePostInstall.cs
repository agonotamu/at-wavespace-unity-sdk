/// @file At_WaveSpacePostInstall.cs
/// @brief Post-install hook for the AT WaveSpace Unity package.
///
/// @details
/// Automatically removes the macOS quarantine extended attribute
/// (com.apple.quarantine) from the native dylib after the .unitypackage
/// is imported. This prevents Gatekeeper from blocking the plugin on
/// machines that do not have the Apple Developer Program certificate.
///
/// Runs automatically via AssetPostprocessor — no user action required.
/// Safe on Windows / Linux : the entire script is skipped on non-macOS editors.

using UnityEditor;
using UnityEngine;
using System.IO;
using System.Diagnostics;

public class At_WaveSpacePostInstall : AssetPostprocessor
{
    private const string DYLIB_NAME  = "at_wavespace_engine.dylib";
    private const string XATTR_PATH  = "/usr/bin/xattr";
    private const string XATTR_ARGS  = "-d com.apple.quarantine";

    static void OnPostprocessAllAssets(
        string[] importedAssets,
        string[] deletedAssets,
        string[] movedAssets,
        string[] movedFromAssetPaths)
    {
        // Only relevant on macOS Editor
        if (Application.platform != RuntimePlatform.OSXEditor) return;

        // Find the dylib among the imported assets
        string dylibAssetPath = null;
        foreach (string path in importedAssets)
        {
            if (path.EndsWith(DYLIB_NAME))
            {
                dylibAssetPath = path;
                break;
            }
        }

        if (dylibAssetPath == null) return;

        string absPath = Path.GetFullPath(dylibAssetPath);

        if (!File.Exists(absPath))
        {
            UnityEngine.Debug.LogWarning(
                $"[AT WaveSpace] Post-install: dylib not found at expected path:\n{absPath}");
            return;
        }

        RemoveQuarantine(absPath);
    }

    // -------------------------------------------------------------------------

    private static void RemoveQuarantine(string absPath)
    {
        // Check first whether the attribute is actually present.
        // xattr -p exits with code 1 if the attribute is absent — not an error.
        if (!HasQuarantineAttribute(absPath))
        {
            UnityEngine.Debug.Log(
                "[AT WaveSpace] Post-install: dylib has no quarantine attribute — nothing to do.");
            return;
        }

        var psi = new ProcessStartInfo
        {
            FileName               = XATTR_PATH,
            Arguments              = $"{XATTR_ARGS} \"{absPath}\"",
            UseShellExecute        = false,
            RedirectStandardOutput = true,
            RedirectStandardError  = true,
            CreateNoWindow         = true
        };

        using (var p = new Process { StartInfo = psi })
        {
            p.Start();
            string stderr = p.StandardError.ReadToEnd();
            p.WaitForExit();

            if (p.ExitCode == 0)
            {
                UnityEngine.Debug.Log(
                    $"[AT WaveSpace] Post-install: quarantine attribute removed from\n{absPath}");
            }
            else
            {
                UnityEngine.Debug.LogWarning(
                    "[AT WaveSpace] Post-install: could not remove quarantine attribute.\n" +
                    (string.IsNullOrEmpty(stderr) ? "" : $"xattr error: {stderr}\n") +
                    "If the plugin fails to load, run this command manually in Terminal:\n" +
                    $"  xattr -d com.apple.quarantine \"{absPath}\"");
            }
        }
    }

    // -------------------------------------------------------------------------

    private static bool HasQuarantineAttribute(string absPath)
    {
        var psi = new ProcessStartInfo
        {
            FileName               = XATTR_PATH,
            Arguments              = $"-p com.apple.quarantine \"{absPath}\"",
            UseShellExecute        = false,
            RedirectStandardOutput = true,
            RedirectStandardError  = true,
            CreateNoWindow         = true
        };

        using (var p = new Process { StartInfo = psi })
        {
            p.Start();
            p.WaitForExit();
            // exit 0 = attribute present, exit 1 = absent
            return p.ExitCode == 0;
        }
    }
}
