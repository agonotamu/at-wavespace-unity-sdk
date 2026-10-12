# AT WaveSpace — enquête de performance (Windows)

Kit de diagnostic dans `tools/perf/` du dépôt AT WaveSpace, sur la
machine Windows. Il est conçu pour être piloté par Claude Code (CLI ou Desktop).

## Symptôme

- Machine : Windows, Intel Core i9 vPro, 64 Go RAM.
- WFS pure, 32 canaux de sortie, pas de virtualisation binaurale, pas de downmix.
- Clics audio dès 8–10 sources. **Augmenter la taille du buffer n'aide pas.**
- Le même moteur tient bien plus de sources sur MacBook Pro M4.

## Ce que le symptôme dit déjà

10 sources × 32 HP = 320 lignes à retard fractionnaire + gains lissés. À 48 kHz, c'est ~15 M
échantillons/s : un ordre de grandeur sous le budget temps réel d'un cœur i9. **Le problème n'est donc très probablement pas le
volume de calcul WFS lui-même.** `tools/perf/bench/microbench.cpp` le démontre sur la machine.

Si un buffer plus grand n'aide pas, le coût n'est pas un débit moyen trop élevé mais :
1. un **blocage** ponctuel dans le callback (lock, attente, I/O, log) dont la durée ne dépend pas du buffer ;
2. ou un coût **par échantillon qui explose** (dénormaux) ;
3. ou un **thread audio mal ordonnancé** (E-core, EcoQoS, plan d'alimentation) ;
4. ou une taille de buffer **qui ne change pas réellement** (ASIO impose la sienne).

## Hypothèses classées (à confirmer / éliminer dans cet ordre)

| # | Hypothèse | Signature attendue dans les mesures | Test discriminant |
|---|---|---|---|
| H0 | Buffer demandé ≠ buffer réel (ASIO ignore `bufferSize`) | `blockSize` du probe constant quel que soit `--buffer` | lire `blockSize`/`sampleRate` du probe |
| H1 | Build Debug / assertions / logs | charge moyenne élevée même à 1 source | `syscheck.ps1` (CRT debug), comparer Release |
| H2 | Dénormaux (x86 seulement — invisibles sur M4) dans NFC/IIR, lisseurs, queues de délais | `denormalHits` > 0, charge qui monte quand des sources décroissent ou sont silencieuses | fichier `decay_to_silence.wav`, option probe `FORCE_FTZ_DAZ` |
| H3 | Lecture fichier / décodage dans le thread audio | pics corrélés au nombre de sources, pire avec fichiers longs ou compressés | `pink_10s.wav` court vs fichier long, WAV vs autre format |
| H4 | Contention : SpinLock / shared_mutex / file HRTF partagés avec le thread principal | `maxUs` ≫ `meanUs`, pics plus fréquents en `--motion orbit` qu'en `static` | `--motion static` vs `orbit`, `--update-hz 0/30/90` |
| H5 | Log émis depuis le thread audio (console Windows lente et bloquante) | `logPerSec` > 0 pendant la lecture | compteur de log du bench |
| H6 | Ordonnancement : thread audio sur E-core, EcoQoS, plan d'alimentation, core parking | `cpu` du probe qui change, cœurs classés E | `--affinity pcore`, `--no-throttle`, plan « Performances optimales » |
| H7 | Recalculs lourds par bloc et par paire (source, HP) : NFC coeffs, normalisation du masque, `std::vector` alloués | charge moyenne ∝ N×32 nettement au-dessus du microbench | sections du probe (`WFS`, `NFC`, `MASK`) + kill switches |
| H7b | Audit statique : masque + délais/gains recalculés **par échantillon** pour chaque paire (source, HP) | section `wfs` ∝ N×32, charge indépendante de la taille du buffer | `--perf blockwfs` |
| H9 | Audit statique : multithreading dès 3 joueurs (attente active du thread audio, workers en priorité normale, état partagé écrit par tous, reprise sur timeout incorrecte) | `mt_wait` proche de 85 % du budget, overruns dès 3 sources | `--perf nomt` |
| H8 | Pilote / DPC | pics même à 1 source, `intervalJitterUs` élevé sans hausse de `callbackUs` | LatencyMon, WASAPI exclusif vs ASIO |

## Protocole

1. **Lire** `.claude/skills/at-spat/SKILL.md` et le code moteur (`AT_SpatializationEngine`,
   `AT_SpatPlayer`, `AT_Spatializer`, `PluginInterface.h/.cpp`).
2. **Vérifier les signatures** de `tools/perf/bench/at_spat_api.h` contre `PluginInterface.h` réel. Les typedefs
   y sont des hypothèses issues de la skill ; corriger ce qui diffère. Ne pas deviner : lire le header.
3. **Audit statique du callback** avant toute mesure. Lister, avec fichier:ligne, tout ce qui est
   atteignable depuis `getNextAudioBlock()` et qui peut bloquer ou allouer :
   locks (`SpinLock`, `CriticalSection`, `std::mutex`, `shared_mutex`), `new`/`std::vector` resize,
   `std::function`, `DBG`/log/callback de log, `AudioFormatReader::read`, `File`, `String` construits,
   `std::cout`, appels trigonométriques par échantillon, calcul de coefficients de filtre par bloc.
   Vérifier la présence de `juce::ScopedNoDenormals` en tête de callback **et** dans les workers.
4. **Instrumenter** le moteur avec `tools/perf/engine/At_PerfProbe.h` en suivant `tools/perf/engine/PATCH.md`
   (tout derrière `AT_SPAT_PERF_PROBE`, comportement inchangé quand la macro est absente).
5. **Construire en Release** le moteur instrumenté (preset `windows-x64-release-probe`, sortie
   `_at_wavespace_engine/Builds/VisualStudio2022_Probe/Release/at_wavespace_engine.dll`, jamais copiée
   dans Unity) et `tools/perf/bench/` (`cmake -S tools/perf/bench -B tools/perf/bench/build` puis
   `cmake --build tools/perf/bench/build --config Release`).
6. Lancer `tools/perf/scripts/syscheck.ps1`, puis `tools\perf\bench\build\Release\at_microbench.exe`.
7. Générer les signaux : `python tools/perf/scripts/gen_test_signals.py signals`.
8. Lancer la matrice : `tools/perf/scripts/run_matrix.ps1 -Dll <dll probe> -Device "<nom>"`
   (`-Quick` : les trois tailles de buffer + `nomt,blockwfs`). Le bench joue du son sur le périphérique.
9. Analyser : `python tools/perf/scripts/analyze.py results/*.csv`.
10. Pour chaque hypothèse confirmée : proposer un correctif minimal, le mesurer avec la même matrice,
    garder les CSV avant/après.

## Règles

- Mesurer en **Release** uniquement. Toute mesure Debug est à jeter.
- Ne pas modifier le comportement audio sans flag ; les kill switches du probe servent à bissecter,
  pas de correctif.
- Rien dans le callback audio : pas d'allocation, pas de lock bloquant, pas d'I/O, pas de log
  synchrone. Le probe lui-même n'utilise que des atomiques relaxées.
- Une hypothèse est confirmée quand le kill switch ou la variante **fait disparaître** les overruns,
  pas quand la charge baisse un peu.
- Garder chaque série de CSV dans `results/<date>_<description>/`.
- Rapport final : tableau hypothèse → verdict → preuve (CSV + chiffres) → correctif proposé.
