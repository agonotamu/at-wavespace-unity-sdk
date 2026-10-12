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
- Outils perf : `tools/perf/` (protocole : `tools/perf/PERF.md`, état de l'instrumentation : `tools/perf/engine/PATCH.md`)
- Scènes et scripts de test ajoutés par Claude : `Assets/At_WaveSpace/TestClaude/`

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

## Build (macOS, Xcode)
- JUCE attendu dans `~/JUCE`. Même principe : `cmake --preset <preset>` puis `cmake --build --preset <preset>`, depuis `_at_wavespace_engine/`.
- Presets : `macos-release` / `macos-debug` (dylib universel x86_64 + arm64, sortie `Builds/MacOSX/`),
  `macos-console-release` / `macos-console-debug` (`TestAppConsole`). Pas de preset probe : ajouter `-DAT_SPAT_PERF_PROBE=ON -DAT_SPAT_COPY_TO_UNITY=OFF`.
- Le post-build copie et signe le dylib dans `Assets/At_WaveSpace/Plugins/MacOSX/`.
- État au 12 oct. 2026 : premier build macOS des correctifs de performance d'octobre fait avec `macos-release`,
  sans aucune modification de source ; le dylib du dépôt contient ce moteur. Validé à l'écoute dans `TestClaude_Orbit` :
  200 sources à 32 canaux (maximum de la scène, limite non atteinte), 140 à 64, 60 à 128, 30 à 256, 15 à 512.
  Machine : MacBook Pro `Mac16,1`, Apple M4 (4 cœurs performance + 6 efficacité), 16 Go, macOS 15.7.2, Unity 6000.3.0f1 ;
  sortie stéréo intégrée en downmix HRTF, 48 kHz, buffer 512.
- Les scripts de `tools/perf/scripts/` sont en PowerShell ; le bench C++ est prévu pour macOS mais n'y a jamais été compilé.

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
- Tout ce que Claude crée côté Unity (scripts, scènes, outils d'éditeur) va dans `Assets/At_WaveSpace/TestClaude/`.
  Seule exception : les fichiers d'état de scène, que le projet ne lit que dans `Assets/StreamingAssets/<scène>_States.state`.
- Validation à l'écoute : scène `TestClaude_Orbit` (N sources de parole en orbite autour de l'auditeur, nombre réglable
  en mode Play). L'utilisateur indique à combien de sources l'audio décroche. Reconstruire la bibliothèque Unity fermé avant un test.
- Le dossier de travail n'est pas un dépôt git : faire une sauvegarde (`saveLib.bat` sous Windows) avant de modifier le moteur.
- Publication : l'utilisateur copie les sources vers son clone de `agonotamu/at-wavespace-unity-sdk` et committe lui-même
  (`updateGitSource.bat` sous Windows, propre à ce poste et jamais copié dans le dépôt). Ne faire ni commit ni push à sa place.
