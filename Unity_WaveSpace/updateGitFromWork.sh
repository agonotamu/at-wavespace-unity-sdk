#!/usr/bin/env bash
# updateGitFromWork.sh : copie les sources du dossier de travail vers le clone Git at-wavespace-unity-sdk.
#
#   _at_wavespace_engine                  moteur natif (sans Builds)
#   Assets/At_WaveSpace                   integration Unity, plugins, TestClaude
#   Assets/StreamingAssets/TestClaude_*   etats des scenes de test uniquement
#   tools/perf                            kit de mesure (sans bench/build)
#   CLAUDE.md, .claude/skills             (sans settings.local.json)
#
# A placer et a lancer dans le dossier de travail Unity_WaveSpace (macOS, ou Git Bash sous Windows) :
#   bash updateGitFromWork.sh [-n] [--delete] [dossier Unity_WaveSpace du clone]
#
#   -n         essai a blanc : montre ce qui serait copie, sans rien ecrire
#   --delete   supprime aussi dans le clone ce qui n'existe plus ici (rsync seulement)
#
# Ne fait ni commit ni push : affiche "git status" a la fin.
# Equivalent de updateGitFromWork.bat ; sens inverse de updateWorkFromGit.sh.

set -u

WORK="$(cd "$(dirname "$0")" && pwd)"
DEST="$WORK/../GitHub/at-wavespace-unity-sdk/Unity_WaveSpace"
DRY=0
DELETE=0

for arg in "$@"; do
    case "$arg" in
        -n|--dry-run) DRY=1 ;;
        --delete)     DELETE=1 ;;
        -h|--help)    sed -n '2,17p' "$0"; exit 0 ;;
        *)            DEST="$arg" ;;
    esac
done

say() { echo "[updateGitFromWork] $*"; }
die() { say "ERREUR : $*"; exit 1; }

[ -d "$DEST" ] || die "dossier cible introuvable : $DEST"
DEST="$(cd "$DEST" && pwd)"
[ -e "$DEST/../.git" ] || die "$DEST/.. n'est pas un depot Git"
[ "$DEST" != "$WORK" ] || die "le clone et le dossier de travail sont le meme dossier"

HAVE_RSYNC=0
command -v rsync >/dev/null 2>&1 && HAVE_RSYNC=1
if [ "$DELETE" -eq 1 ] && [ "$HAVE_RSYNC" -eq 0 ]; then
    die "--delete demande rsync, absent ici (Git Bash) : copie par-dessus uniquement"
fi

say "$WORK  -->  $DEST"
[ "$DRY" -eq 1 ] && say "ESSAI A BLANC : rien n'est ecrit"

# copy_dir <chemin relatif> [dossier a exclure, relatif a ce chemin]
copy_dir() {
    local rel="$1" exclude="${2:-}"
    [ -d "$WORK/$rel" ] || die "dossier source introuvable : $WORK/$rel"

    if [ "$HAVE_RSYNC" -eq 1 ]; then
        # Options limitees a celles que connaissent rsync 2.6.9 et openrsync (macOS).
        local opts=(-a --exclude .DS_Store)
        if [ "$DRY" -eq 1 ];    then opts+=(-n -v); fi
        if [ "$DELETE" -eq 1 ]; then opts+=(--delete); fi
        if [ -n "$exclude" ];   then opts+=(--exclude "/$exclude"); fi
        if [ "$DRY" -eq 0 ];    then mkdir -p "$DEST/$rel"; fi
        rsync "${opts[@]}" "$WORK/$rel/" "$DEST/$rel/" | sed 's/^/    /'
        [ "${PIPESTATUS[0]}" -eq 0 ] || die "copie de $rel"
        say "OK : $rel"
    else
        # Sans rsync (Git Bash) : copie par-dessus avec tar.
        local excl="--exclude=./__aucune_exclusion__"
        if [ -n "$exclude" ]; then excl="--exclude=./$exclude"; fi
        if [ "$DRY" -eq 1 ]; then
            local n
            n="$(cd "$WORK/$rel" && tar cf - "$excl" . | tar tf - | grep -vc '/$' || true)"
            say "OK : $rel ($n fichier(s) seraient copies)"
        else
            mkdir -p "$DEST/$rel"
            (cd "$WORK/$rel" && tar cf - "$excl" .) | (cd "$DEST/$rel" && tar xf -) || die "copie de $rel"
            say "OK : $rel"
        fi
    fi
}

copy_dir "_at_wavespace_engine"  "Builds"
copy_dir "Assets/At_WaveSpace"
copy_dir "tools/perf"            "bench/build"
copy_dir ".claude/skills"

# Etats des scenes de test : jamais de purge ici, StreamingAssets contient d'autres fichiers.
found=0
for f in "$WORK"/Assets/StreamingAssets/TestClaude_*_States.state*; do
    [ -f "$f" ] || continue
    found=1
    if [ "$DRY" -eq 1 ]; then
        echo "    $(basename "$f")"
    else
        mkdir -p "$DEST/Assets/StreamingAssets"
        cp -p "$f" "$DEST/Assets/StreamingAssets/" || die "copie de $(basename "$f")"
    fi
done
if [ "$found" -eq 1 ]; then
    say "OK : Assets/StreamingAssets/TestClaude_*_States.state"
else
    say "aucun etat TestClaude a copier"
fi

[ -f "$WORK/CLAUDE.md" ] || die "CLAUDE.md introuvable dans $WORK"
if [ "$DRY" -eq 0 ]; then
    cp -p "$WORK/CLAUDE.md" "$DEST/CLAUDE.md" || die "copie de CLAUDE.md"
fi
say "OK : CLAUDE.md"

echo
if [ "$DRY" -eq 1 ]; then
    say "Essai termine : rien n'a ete modifie dans le clone."
elif command -v git >/dev/null 2>&1; then
    say "Etat du clone (aucun commit ni push n'a ete fait) :"
    git -C "$DEST/.." status --short
fi
