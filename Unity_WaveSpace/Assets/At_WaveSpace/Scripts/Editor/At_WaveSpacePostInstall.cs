/// @file At_WaveSpacePostInstall.cs
/// @brief Post-install hook for the AT WaveSpace Unity package.
///
/// @details
/// Removes the quarantine extended attribute (com.apple.quarantine) and
/// re-signs the native dylib with an ad-hoc signature. Both steps are required
/// on macOS Tahoe (15.x): extracting a .unitypackage invalidates the existing
/// signature, and Tahoe refuses to load any dylib whose signature is missing
/// or invalid — even without quarantine.
///
/// Two trigger mechanisms are combined in this file:
///   1. AssetPostprocessor.OnPostprocessAllAssets — fires when the
///      .unitypackage is first imported (fresh install).
///   2. [InitializeOnLoad] static constructor — fires on every Unity domain
///      reload (project open, recompile), covering projects where the dylib
///      is already present in Assets/.
///
/// A manual menu item (AT WaveSpace / Fix macOS Codesign) is also provided
/// so the end-user can trigger the fix explicitly if needed.
///
/// Safe on Windows / Linux: the entire script is skipped on non-macOS editors.

#if UNITY_EDITOR && UNITY_EDITOR_OSX
using UnityEditor;
using UnityEngine;
using System.IO;
using System.Diagnostics;

[InitializeOnLoad]
public class At_WaveSpacePostInstall : AssetPostprocessor
{
    private const string DYLIB_ASSET_PATH =
        "Assets/At_WaveSpace/Plugins/MacOSX/at_wavespace_engine.dylib";
    private const string DYLIB_NAME = "at_wavespace_engine.dylib";

    // ── InitializeOnLoad — runs on every domain reload ───────────────────────
    static At_WaveSpacePostInstall()
    {
        FixDylib(Path.GetFullPath(DYLIB_ASSET_PATH));
    }

    // ── Manual menu item ─────────────────────────────────────────────────────
    [MenuItem("AT WaveSpace/Fix macOS Codesign (Tahoe)")]
    public static void RunManualFix()
    {
        FixDylib(Path.GetFullPath(DYLIB_ASSET_PATH));
    }

    // ── AssetPostprocessor — runs on fresh .unitypackage import ──────────────
    static void OnPostprocessAllAssets(
        string[] importedAssets,
        string[] deletedAssets,
        string[] movedAssets,
        string[] movedFromAssetPaths)
    {
        foreach (string path in importedAssets)
        {
            if (path.EndsWith(DYLIB_NAME))
            {
                FixDylib(Path.GetFullPath(path));
                break;
            }
        }
    }

    // ── Core fix logic ───────────────────────────────────────────────────────
    private static void FixDylib(string absPath)
    {
        if (!File.Exists(absPath))
        {
            // Silently ignore — dylib may not be present on this platform.
            return;
        }

        // Step 1 — Remove quarantine attribute (harmless if absent).
        RunCommand("/usr/bin/xattr", $"-r -d com.apple.quarantine \"{absPath}\"");

        // Step 2 — Strip any broken / expired signature.
        RunCommand("/usr/bin/codesign", $"--remove-signature \"{absPath}\"");

        // Step 3 — Re-sign with an ad-hoc identity ("-").
        // Required on Tahoe (15.x): an unsigned dylib is also rejected.
        RunCommand("/usr/bin/codesign", $"--sign - --force --deep \"{absPath}\"");

        UnityEngine.Debug.Log(
            "[AT WaveSpace] macOS codesign fix applied to at_wavespace_engine.dylib");
    }

    // ── Shell helper ─────────────────────────────────────────────────────────
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
            string err = p.StandardError.ReadToEnd().Trim();
            p.WaitForExit();

            if (p.ExitCode != 0 && !string.IsNullOrEmpty(err))
                UnityEngine.Debug.LogWarning(
                    $"[AT WaveSpace] {exe} {args}\n→ {err}");
        }
    }
}
#endif
