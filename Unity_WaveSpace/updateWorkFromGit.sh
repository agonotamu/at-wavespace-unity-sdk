#!/usr/bin/env bash
# updateWorkFromGit.sh : met a jour le dossier de travail Unity_WaveSpace a partir du clone Git.
#
#   .claude/skills, *.md (CLAUDE.md...)   instructions et skill pour Claude Code
#   tools/perf                            kit de mesure (sans bench/build)
#   _at_wavespace_engine                  moteur natif (sans Builds)
#   Assets/At_WaveSpace                   integration Unity, plugins, TestClaude
#   Assets/StreamingAssets                audio, HRTF, etats des scenes
#
# A placer et a lancer dans le dossier de travail Unity_WaveSpace (macOS, ou Git Bash sous Windows) :
#   bash updateWorkFromGit.sh [-n] [--delete] [--force] [dossier Unity_WaveSpace du clone]
#
#   -n         essai a blanc : montre ce qui serait copie, sans rien ecrire
#   --delete   supprime aussi ici ce qui n'existe plus dans le clone (rsync seulement)
#   --force    copie meme si Unity est ouvert
#
# Ne fait ni "git pull" ni commit : mettre le clone a jour avant de lancer le script.
# Attention : les fichiers locaux sont remplaces par ceux du clone, y compris les etats de
# scene de StreamingAssets (peripherique audio, taille de buffer...).

set -u

WORK="$(cd "$(dirname "$0")" && pwd)"
SRC="$WORK/../GitHub/at-wavespace-unity-sdk/Unity_WaveSpace"
DRY=0
DELETE=0
FORCE=0

for arg in "$@"; do
    case "$arg" in
        -n|--dry-run) DRY=1 ;;
        --delete)     DELETE=1 ;;
        --force)      FORCE=1 ;;
        -h|--help)    sed -n '2,20p' "$0"; exit 0 ;;
        *)            SRC="$arg" ;;
    esac
done

say() { echo "[updateWorkFromGit] $*"; }
die() { say "ERREUR : $*"; exit 1; }

[ -d "$SRC" ] || die "dossier du clone introuvable : $SRC"
SRC="$(cd "$SRC" && pwd)"
[ -d "$SRC/_at_wavespace_engine" ] || die "$SRC ne ressemble pas au dossier Unity_WaveSpace du clone"
[ "$SRC" != "$WORK" ] || die "le clone et le dossier de travail sont le meme dossier"

# Unity verrouille (Windows) ou a deja charge (macOS) la bibliotheque native.
unity_running() {
    if command -v pgrep >/dev/null 2>&1; then
        pgrep -x Unity >/dev/null 2>&1 && return 0
    fi
    if command -v tasklist >/dev/null 2>&1; then
        tasklist 2>/dev/null | grep -qi '^Unity\.exe' && return 0
    fi
    return 1
}
if [ "$DRY" -eq 0 ] && [ "$FORCE" -eq 0 ] && unity_running; then
    die "Unity est ouvert : le fermer avant la copie, ou relancer avec --force"
fi

HAVE_RSYNC=0
command -v rsync >/dev/null 2>&1 && HAVE_RSYNC=1
if [ "$DELETE" -eq 1 ] && [ "$HAVE_RSYNC" -eq 0 ]; then
    die "--delete demande rsync, absent ici (Git Bash) : copie par-dessus uniquement"
fi

say "$SRC  -->  $WORK"
if command -v git >/dev/null 2>&1; then
    say "clone : $(git -C "$SRC" log -1 --format='%ad  %s' --date=short 2>/dev/null || echo 'commit inconnu')"
fi
[ "$DRY" -eq 1 ] && say "ESSAI A BLANC : rien n'est ecrit"

# copy_dir <chemin relatif> [dossier a exclure, relatif a ce chemin]
copy_dir() {
    local rel="$1" exclude="${2:-}"
    if [ ! -d "$SRC/$rel" ]; then
        say "absent du clone, ignore : $rel"
        return 0
    fi

    if [ "$HAVE_RSYNC" -eq 1 ]; then
        # Options limitees a celles que connaissent rsync 2.6.9 et openrsync (macOS).
        local opts=(-a)
        if [ "$DRY" -eq 1 ];    then opts+=(-n -v); fi
        if [ "$DELETE" -eq 1 ]; then opts+=(--delete); fi
        if [ -n "$exclude" ];   then opts+=(--exclude "/$exclude"); fi
        if [ "$DRY" -eq 0 ];    then mkdir -p "$WORK/$rel"; fi
        rsync "${opts[@]}" "$SRC/$rel/" "$WORK/$rel/" | sed 's/^/    /'
        [ "${PIPESTATUS[0]}" -eq 0 ] || die "copie de $rel"
        say "OK : $rel"
    else
        # Sans rsync (Git Bash) : copie par-dessus avec tar.
        local excl="--exclude=./__aucune_exclusion__"
        if [ -n "$exclude" ]; then excl="--exclude=./$exclude"; fi
        if [ "$DRY" -eq 1 ]; then
            local n
            n="$(cd "$SRC/$rel" && tar cf - "$excl" . | tar tf - | grep -vc '/$' || true)"
            say "OK : $rel ($n fichier(s) seraient copies)"
        else
            mkdir -p "$WORK/$rel"
            (cd "$SRC/$rel" && tar cf - "$excl" .) | (cd "$WORK/$rel" && tar xf -) || die "copie de $rel"
            say "OK : $rel"
        fi
    fi
}

copy_dir ".claude/skills"
copy_dir "tools/perf"            "bench/build"
copy_dir "_at_wavespace_engine"  "Builds"
copy_dir "Assets/At_WaveSpace"
copy_dir "Assets/StreamingAssets"

# Fichiers .md a la racine du projet (CLAUDE.md, INSTALL.md...)
for f in "$SRC"/*.md; do
    [ -f "$f" ] || continue
    if [ "$DRY" -eq 1 ]; then
        say "OK : $(basename "$f") (serait copie)"
    else
        cp -p "$f" "$WORK/" || die "copie de $(basename "$f")"
        say "OK : $(basename "$f")"
    fi
done

if [ "$DRY" -eq 1 ]; then
    say "Essai termine : rien n'a ete modifie."
else
    say "Termine. Reconstruire la bibliotheque native si les sources du moteur ont change."
fi
