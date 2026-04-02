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
    private const string DYLIB_NAME    = "at_wavespace_engine.dylib";
    private const string XATTR_PATH    = "/usr/bin/xattr";
    private const string XATTR_ARGS    = "-d com.apple.quarantine";

    // SessionState key — persists across assembly reloads within an Editor session.
    // A static bool would be reset when SaveAndReimport() triggers a domain reload.
    private const string SESSION_KEY   = "AT_WaveSpace_PostInstall_Processing";

    // ── Debug menu ───────────────────────────────────────────────────────────
    [MenuItem("AT_WaveSpace/Debug/Test Post-Install")]
    static void TestPostInstall()
    {
        SessionState.EraseBool(SESSION_KEY); // reset guard for manual test
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

        // Prevent re-entry from the SaveAndReimport() call below.
        // SessionState survives domain reloads — a static bool would not.
        if (SessionState.GetBool(SESSION_KEY, false)) return;

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
        if (!File.Exists(absPath)) return;

        SessionState.SetBool(SESSION_KEY, true);
        try
        {
            // Step 1 — Remove invalid ad-hoc signature
            RunCommand("/usr/bin/codesign", $"--remove-signature \"{absPath}\"");

            // Step 2 — Remove quarantine attribute
            if (HasQuarantineAttribute(absPath))
                RunCommand(XATTR_PATH, $"{XATTR_ARGS} \"{absPath}\"");

            // Step 3 — Force Unity to reload the now-clean dylib
            PluginImporter importer = AssetImporter.GetAtPath(dylibAssetPath) as PluginImporter;
            if (importer != null)
            {
                importer.SetCompatibleWithEditor(true);
                importer.SaveAndReimport();
            }
        }
        finally
        {
            SessionState.EraseBool(SESSION_KEY);
        }
    }

    // ── Helpers ──────────────────────────────────────────────────────────────
    private static void RunCommand(string exe, string args)
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
            p.StandardOutput.ReadToEnd();
            p.StandardError.ReadToEnd();
            p.WaitForExit();
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
            return p.ExitCode == 0;
        }
    }
}
