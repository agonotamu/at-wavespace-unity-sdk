/// @file At_WaveSpacePostInstall.cs
/// @brief Post-install hook for the AT WaveSpace Unity package.
///
/// @details
/// Automatically removes the invalid ad-hoc code signature and the macOS
/// quarantine extended attribute (com.apple.quarantine) from the native dylib
/// after the .unitypackage is imported, then forces Unity to reimport the
/// plugin so the cleaned dylib is loaded immediately.
///
/// Runs automatically via AssetPostprocessor — no user action required.
/// Safe on Windows / Linux : the entire script is skipped on non-macOS editors.

using UnityEditor;
using UnityEngine;
using System.IO;
using System.Diagnostics;

public class At_WaveSpacePostInstall : AssetPostprocessor
{
    private const string DYLIB_NAME = "at_wavespace_engine.dylib";
    private const string XATTR_PATH = "/usr/bin/xattr";
    private const string XATTR_ARGS = "-d com.apple.quarantine";

    // ── Debug menu ───────────────────────────────────────────────────────────
    [MenuItem("AT_WaveSpace/Debug/Test Post-Install")]
    static void TestPostInstall()
    {
        string[] fakeImport = {
            "Assets/At_WaveSpace/Plugins/MacOSX/at_wavespace_engine.dylib"
        };
        OnPostprocessAllAssets(fakeImport,
            new string[0], new string[0], new string[0]);
    }

    // ── AssetPostprocessor entry point ───────────────────────────────────────
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
                $"[AT WaveSpace] Post-install: dylib not found at:\n{absPath}");
            return;
        }

        // Step 1 — Remove invalid ad-hoc signature (blocked by Unity Hardened Runtime)
        RunCommand("/usr/bin/codesign",
                   $"--remove-signature \"{absPath}\"",
                   "code signature removed",
                   "could not remove code signature");

        // Step 2 — Remove quarantine attribute (blocked by Gatekeeper)
        if (HasQuarantineAttribute(absPath))
            RunCommand(XATTR_PATH,
                       $"{XATTR_ARGS} \"{absPath}\"",
                       "quarantine attribute removed",
                       "could not remove quarantine attribute");
        else
            UnityEngine.Debug.Log("[AT WaveSpace] Post-install: no quarantine attribute — skipping.");

        // Step 3 — Force Unity to reload the dylib now that it is clean
        PluginImporter importer = AssetImporter.GetAtPath(dylibAssetPath) as PluginImporter;
        if (importer != null)
        {
            importer.SetCompatibleWithEditor(true);
            importer.SaveAndReimport();
            UnityEngine.Debug.Log("[AT WaveSpace] Post-install: plugin reimported — ready to use.");
        }
        else
        {
            UnityEngine.Debug.LogWarning(
                "[AT WaveSpace] Post-install: could not get PluginImporter.\n" +
                "Right-click the dylib in the Project window and select Reimport.");
        }
    }

    // ── Helpers ──────────────────────────────────────────────────────────────
    private static void RunCommand(string exe, string args,
                                   string successMsg, string failMsg)
    {
        var psi = new ProcessStartInfo
        {
            FileName               = exe,
            Arguments              = args,
            UseShellExecute        = false,
            RedirectStandardOutput = true,
            RedirectStandardError  = true,
            CreateNoWindow         = true
        };

        using (var p = new Process { StartInfo = psi })
        {
            p.Start();
            string stdout = p.StandardOutput.ReadToEnd();
            string stderr = p.StandardError.ReadToEnd();
            p.WaitForExit();

            if (p.ExitCode == 0)
                UnityEngine.Debug.Log($"[AT WaveSpace] Post-install: {successMsg}.");
            else
                UnityEngine.Debug.LogWarning(
                    $"[AT WaveSpace] Post-install: {failMsg}.\n" +
                    (string.IsNullOrEmpty(stderr) ? "" : $"stderr: {stderr}"));
        }
    }

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
