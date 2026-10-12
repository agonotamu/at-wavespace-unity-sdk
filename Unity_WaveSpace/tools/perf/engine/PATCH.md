# At_PerfProbe dans le moteur — état de l'intégration (oct. 2026)

Tout est derrière `AT_SPAT_PERF_PROBE`. Sans la macro, `At_PerfProbe.h` ne déclare que des macros
vides et le moteur est inchangé.

`At_PerfProbe.h` existe en deux exemplaires qui doivent rester **identiques** (la structure `StatsC`
est partagée par ABI entre la DLL et le bench) :

- `tools/perf/engine/At_PerfProbe.h` — référence, incluse par le bench ;
- `_at_wavespace_engine/Source/At_PerfProbe.h` — incluse par le moteur.

Après toute modification : recopier, puis reconstruire le moteur **et** le bench. Le bench refuse de
tourner si `StatsC.version` ne correspond pas.

## 1. CMake (`_at_wavespace_engine/CMakeLists.txt`, `CMakePresets.json`)

- `option(AT_SPAT_PERF_PROBE … OFF)` → `target_compile_definitions(at_wavespace_engine PRIVATE AT_SPAT_PERF_PROBE=1)`.
- `option(AT_SPAT_COPY_TO_UNITY … ON)` : à `OFF`, l'étape post-build ne copie plus la DLL dans
  `Assets/At_WaveSpace/Plugins`.
- Preset `windows-x64-release-probe` : probe `ON`, copie Unity `OFF`, sortie dans
  `_at_wavespace_engine/Builds/VisualStudio2022_Probe/Release/at_wavespace_engine.dll`.

```powershell
cd _at_wavespace_engine
cmake --preset windows-x64-release-probe
cmake --build --preset windows-x64-release-probe --config Release
```

Les options de compilation sont celles du build normal (on mesure ce qui est livré).

## 2. Callback audio — `SpatializationEngine::getNextAudioBlock()` (`AT_SpatializationEngine.cpp`)

En tête : `AT_PERF_CALLBACK_SCOPE (bufferToFill.numSamples, m_sampleRate);` puis `AT_PERF_FTZ_IF_REQUESTED();`.

`juce::ScopedNoDenormals` a été ajouté **après** mesure (voir § 6) : en tête du callback et dans les
deux lambdas des workers. `--perf ftz` n'a donc plus d'effet mesurable ; `ftz_daz_off` doit rester à 0
dans les CSV.

Les jobs des workers (`processPlayersWFS`, `processChunk` HRTF) commencent par `AT_PERF_WORKER_SCOPE();` :
FTZ/DAZ sur demande, et un `denormalHits` par job ayant rencontré une opérande dénormale (le flag
MXCSR est par thread).

## 3. Sections

`sectionMeanUs` = temps cumulé **par callback**, tous threads confondus (en multithread, c'est du
temps CPU : il peut dépasser la durée du callback). `sectionMaxUs` = plus longue occurrence isolée.

| Section | Où | Ce qu'elle couvre |
|---|---|---|
| `players` | `SpatPlayer::updateForNextBlock()` | lecture fichier + resampling + gain + préfiltre + RMS, par joueur |
| `wfs` | `SpatPlayer::processAndAdd()` | boucle par échantillon : masque, délais/gains, ligne à retard, par joueur |
| `nfc` | `getNextAudioBlock` / `processBinauralVirtualization` | géométrie NFC + `processStereo` |
| `mask` | — | non utilisée : le masque est calculé par échantillon à l'intérieur de `wfs` ; utiliser `nomask` / `blockwfs` |
| `hrtf` | `getNextAudioBlock` | `processBinauralVirtualization()` |
| `output` | `getNextAudioBlock` étapes 5–6 | fade de transition, gain master (`powf` par échantillon), vumètres |
| `locks` | `getNextAudioBlock` | `m_playerListLock` (×2) et `m_positionLock` (×4) : acquisition **et** la copie protégée (quelques Ko) |
| `other` | `getNextAudioBlock` étape 1 | destruction différée des joueurs retirés |
| `mt_dispatch` | `processPlayersWFS` | boucle `ThreadPool::addJob` (allocation + lock du pool) |
| `mt_wait` | `processPlayersWFS` | le thread audio traite lui-même des joueurs, puis attend ceux qu'un worker a en cours |

Le maximum de `locks` est le chiffre utile : la copie est de l'ordre de la microseconde, tout pic
au-delà est de l'attente.

## 4. Kill switches (`--perf …` du bench)

| Option | Bench | Effet | Emplacement |
|---|---|---|---|
| `OPT_FORCE_FTZ_DAZ` | `ftz` | FTZ+DAZ sur le thread audio et les workers | macros de portée |
| `OPT_BYPASS_NFC` | `nonfc` | pas de NFC | `AT_SpatializationEngine.cpp` (3 conditions) |
| `OPT_BYPASS_MASK_NORM` | `nomask` | masque forcé « tous HP actifs », pas de classification | `Spatializer::setIsInsideAndUpdateSpeakerMask()` |
| `OPT_FREEZE_WFS_PARAMS` | `freeze` | délais/gains calculés une fois puis gelés | `Spatializer::perfUpdateWfsGainAndDelay()` |
| `OPT_MUTE_LOG_AUDIO` | `mutelog` | supprime les logs émis par le thread audio | `UnityLogger.cpp` (`AT_PERF_AUDIO_LOG_GUARD`) |
| `OPT_SKIP_FILE_READ` | `noread` | bruit blanc au lieu de la lecture fichier | `SpatPlayer::updateForNextBlock()` |
| `OPT_NO_POSITION_LOCK` | `nolock` | le callback lit les positions sans `m_positionLock` | `AT_PERF_POSITION_LOCK` (4 sites) |
| `OPT_NO_MULTITHREADING` | `nomt` | tous les joueurs sur le thread audio | `processPlayersWFS`, `processBinauralVirtualization` |
| `OPT_WFS_PARAMS_PER_BLOCK` | `blockwfs` | masque + délais/gains une fois par bloc | `Spatializer::perfAdvanceSourceSmoothersPerBlock()` |
| `OPT_GLOBAL_SMOOTHERS_PER_BLOCK` | `gsblock` | **sans effet** : c'est devenu le comportement normal (§ 6). Valeur réservée | — |
| `OPT_FORCE_PLAYER_MT` | `mt` | force le multithreading des joueurs ; redondant depuis qu'il est actif par défaut | `processPlayersWFS` |

Ce sont des outils de bissection, pas des correctifs : `freeze`, `blockwfs`, `nomask`, `noread` et
`nolock` modifient le rendu (délais en escalier, masque faux, bruit, lectures déchirées possibles).

Logs : `DISABLE_UNITY_LOGGING` étant défini (`AT_SpatConfig.h`, `CMakeLists.txt`), les `LOG` sont
compilés à vide et `logCallsFromAudio` reste à 0 — c'est la preuve attendue pour H5. Le compteur ne
devient utile que si l'on réactive les logs.

## 5. Exports C (`PluginInterface.h` / `.cpp`, sous `#if defined(AT_SPAT_PERF_PROBE)`)

```cpp
EXPORT_API int CALL_CONV AT_WS_getPerfStats (void* outStats, int sizeBytes);   // at_perf::StatsC*
EXPORT_API int CALL_CONV AT_WS_resetPerfStats();
EXPORT_API int CALL_CONV AT_WS_setPerfOptions (unsigned int options);
```

Sans `g_mutex` (atomiques relaxées uniquement). Le bench les charge par `GetProcAddress` et
fonctionne en mode dégradé si elles sont absentes.

## 6. Correctifs appliqués au moteur après mesure (12 oct. 2026)

Ces changements ne sont pas derrière la macro : ils font partie du moteur.

| Correctif | Fichiers | Preuve |
|---|---|---|
| `ScopedNoDenormals` (callback + workers) | `AT_SpatializationEngine.cpp` | une source qui s'éteint : 21,1 % → 4,7 % de charge (`results/20261012_1100_separation`, `…_1123_fix1_fix2`) |
| Lisseurs globaux avancés par le moteur seul, une fois par bloc (`advanceGlobalSmoothersForBlock`), lus par interpolation par les joueurs (`getBlockInterpolatedVirtualSpeakerPosition`, `Spatializer::beginBlock`) | `AT_SpatializationEngine.h/.cpp`, `AT_Spatializer.h/.cpp`, `AT_SpatPlayer.cpp` | test `gsblock` : 24 sources à 28 % (`…_1154_etat_partage`) |
| Jobs par joueur à réclamation unique (`m_playerJobState`, `tryProcessPlayerJob`), thread audio qui travaille, plus de délai de 85 % ni de capture par référence | `AT_SpatializationEngine.h/.cpp` | 32 sources à 48 %, aucun overrun ; séquentiel : overruns à 12 (`…_1322_mt_corrige`) |
| Verrou `m_sourceLock` autour du remplacement du lecteur d'un joueur | `AT_SpatPlayer.h/.cpp` | plantage (division par zéro dans `juce::ResamplingAudioSource`) à l'ajout d'une source à 256 HP virtuels ; disparu au bench et dans Unity |

À l'écoute dans `Assets/MyAssets/TestClaude/TestClaude_Orbit.unity` (downmix HRTF) : environ 10 sources
avant, 18–19 avec les deux premiers correctifs seuls (multithreading des joueurs désactivé), puis
100 à 32 canaux, 48 à 64 canaux et 20 à 128 canaux avec l'ensemble.

Non traité : paramètres WFS par bloc (`blockwfs`), lecture des fichiers hors du callback, défauts du
chemin multithread HRTF (paquet du thread audio vide à 32 HP, sortie des workers abandonnée sur
dépassement).

## 7. Relevés de l'audit statique à confirmer par la mesure

- Taille réelle du bloc : `block_size` du CSV (le moteur ne vérifie pas ce que le pilote a accepté).
- Priorité du thread audio : `thread_prio` du CSV ; les workers du `juce::ThreadPool` sont en priorité normale.
- Lecture fichier : `AudioTransportSource` sans tampon d'avance ni thread (`setSource (…, 0, nullptr, …)`),
  donc lecture disque dans le callback ou dans un worker.
