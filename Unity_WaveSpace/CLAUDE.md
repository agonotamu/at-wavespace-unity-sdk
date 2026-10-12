# AT WaveSpace — instructions projet pour Claude Code

Langue de travail : **français** (code et commentaires : garder le style existant du fichier).

## Contexte
Moteur de spatialisation temps réel (WFS, binaural HRTF, Simple Binaural A/B, 6DOF) en C++/JUCE,
exposé en C (`extern "C"`, préfixe `AT_WS_`, WS = WaveSpace) et intégré dans Unity via P/Invoke.
Référence détaillée : `.claude/skills/at-spat/SKILL.md` — la lire avant toute modification du moteur
ou de l'intégration Unity.

## Dossiers
<!-- À COMPLÉTER au premier lancement : Claude Code, remplis ces chemins en explorant le dépôt. -->
- Lib C++ : `_at_wavespace_engine/` (sources dans `Source/`, `CMakeLists.txt`, `CMakePresets.json` ; JUCE attendu dans `C:/JUCE`)
- Header d'export C : `_at_wavespace_engine/Source/PluginInterface.h` (implémentation : `PluginInterface.cpp`)
- Projet Unity (scripts C#) : racine du dépôt ; scripts dans `Assets/At_WaveSpace/Scripts/Engine` et `Assets/At_WaveSpace/Scripts/Editor`
- DLL copiée dans Unity : `Assets/At_WaveSpace/Plugins/Win/at_wavespace_engine.dll` (macOS : `Plugins/MacOSX/at_wavespace_engine.dylib`),
  copie faite automatiquement par l'étape post-build de `_at_wavespace_engine/CMakeLists.txt`
- Sorties de build : `_at_wavespace_engine/Builds/VisualStudio2022/` (DLL), `Builds/VisualStudio2022_Console/` (console)
- À ignorer : `Library/`, `Temp/`, `Logs/`, `obj/`, `_at_wavespace_engine/Builds/`, `_backup/`
- Outils perf : `tools/perf/` (protocole : `tools/perf/PERF.md`)

## Build (Windows, Visual Studio 2022)
- Lancer Claude Code depuis **Developer PowerShell for VS 2022** (MSVC + CMake dans le PATH).
- Toujours configurer avant de construire : `cmake --preset <preset>` puis `cmake --build --preset <preset> --config Release`.
  À lancer depuis `_at_wavespace_engine/`. Presets Windows (configure et build portent le même nom) :
  - `windows-x64-release` — DLL pour Unity (preset de référence pour les mesures)
  - `windows-x64-release-probe` — DLL instrumentée (`AT_SPAT_PERF_PROBE`), dans `Builds/VisualStudio2022_Probe/`,
    **non copiée** dans Unity ; c'est celle que charge le bench `tools/perf/bench`
  - `windows-x64-debug` — DLL Debug (même dossier de build que le Release)
  - `windows-x86-release` — DLL Win32
  - `windows-console-x64-release` / `windows-console-x64-debug` — `TestAppConsole` (`BUILD_CONSOLE_APP=ON`)
- Le build de la DLL la copie lui-même dans `Assets/At_WaveSpace/Plugins/Win` (post-build) : construire, c'est déjà écraser
  (sauf avec `-DAT_SPAT_COPY_TO_UNITY=OFF`, ce que fait le preset probe).
- **Mesures de performance : Release uniquement.**
- L'éditeur Unity verrouille la DLL : fermer Unity avant de reconstruire, ou recopier la DLL ensuite.
  Ne jamais écraser la DLL dans `Assets/Plugins` pendant que Unity tourne.

## Règles temps réel (callback audio `getNextAudioBlock` et tout ce qu'il appelle)
- Aucune allocation, aucun lock bloquant, aucune I/O, aucun log synchrone.
- `juce::ScopedNoDenormals` en tête du callback et dans chaque thread worker DSP.
- Lissage : `juce::LinearSmoothedValue`. Positions HP : double buffer + flag atomique.
- Ne pas réintroduire les bugs déjà corrigés (liste dans la skill : races `prepare()`, SpinLock positions,
  inversion de priorité, captures lambda pendantes, `shared_mutex` détruit en usage).

## Façon de travailler
- Avant de modifier : expliquer le diagnostic et le correctif envisagé, fichier:ligne à l'appui.
- Changements minimaux et ciblés ; toute instrumentation derrière une macro (`AT_SPAT_PERF_PROBE`).
- Garder la cohérence C++ ↔ C# (`MAX_VIRTUAL_SPEAKERS = 1024`, signatures d'export, return codes 0/1).
- Ne pas committer sans demande explicite.
