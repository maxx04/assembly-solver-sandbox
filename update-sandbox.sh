#!/usr/bin/env bash
#
# update-sandbox.sh
#
# Synchronisiert diese Sandbox (Sparse-Checkout von src/Mod/Assembly +
# src/3rdParty/OndselSolver) mit einem neueren Commit von
# github.com/FreeCAD/FreeCAD - siehe SANDBOX_NOTES.md, Abschnitt
# "Sync mit Upstream". Wendet danach automatisch alle Patches aus
# patches/PATCHES.txt neu an (Nutzerauftrag 2026-09-07) - Ablauf analog zu
# FCProjects patches/update-and-rebuild-freecad.sh: Patches erst ENTFERNEN
# (sonst wuerde Schritt 1 sie als "unerwartete lokale Aenderungen" werten und
# der Sync bricht ab), dann syncen, dann neu anwenden.
#
# FCPROJECT-PATCH (2026-10-03, Nutzerauftrag "sandbox soll eigene source benutzen" - live
# aufgetreten: FCProjects VOELLIG SEPARATES update-and-rebuild-freecad.sh hat
# /home/maxx/freecad/freecad-source weiterbewegt, wodurch ein DANACH ohne Zielcommit gestartetes
# update-sandbox.sh hier automatisch auf diesen - fuer UNS nie geplanten - neuen Stand resyncte):
# der Zielcommit-Default kommt jetzt AUSSCHLIESSLICH aus UNSEREM EIGENEN "origin"-Remote (frisch
# gefetcht, siehe Schritt 1/9 unten) - KEIN externes Verzeichnis (weder FCProjects eigener
# freecad-source-Checkout noch unser eigener, oft veralteter Build-Baum unter
# /home/maxx/freecad-sandbox/freecad-source - LETZTERER scheidet ohnehin aus, weil dessen
# src/Mod/Assembly + src/3rdParty/OndselSolver selbst nur Symlinks HIERHER sind, siehe
# SANDBOX_BUILD_DIR weiter unten) wird dafuer noch gelesen. Dadurch kann ein Update in einem
# komplett anderen, nur zufaellig aehnlich benannten Projekt unseren Sync-Zielpunkt nicht mehr
# beeinflussen.
#
# Ablauf:
#   1. git fetch origin - danach Zielcommit-Default (falls keiner als Argument angegeben wurde):
#      HEAD von origin/main in UNSEREM EIGENEN Repo, kein externes Verzeichnis involviert.
#   2. Alle von patches/PATCHES.txt betroffenen Dateien auf den zuletzt
#      committeten Stand zuruecksetzen (git checkout --), damit sie den
#      Sync nicht blockieren. Patches selbst bleiben unangetastet (liegen
#      als eigene Dateien in patches/, nicht betroffen).
#   3. Sicherstellen, dass danach keine UNERWARTETEN lokalen Aenderungen an
#      getrackten Dateien mehr vorliegen - Abbruch statt stillschweigendem
#      Ueberschreiben. Bekannte, harmlose Submodul-Dirty-Eintraege
#      (OndselSolver, falls dort gerade experimentiert wird) werden
#      ausgefiltert statt als Fehler behandelt.
#   4. src/Mod/Assembly + src/3rdParty/OndselSolver per PFAD-BESCHRAENKTEM
#      "git checkout <Zielcommit> -- <pfade>" auf den Zielcommit bringen -
#      NIEMALS ein volles "git checkout <Zielcommit>" (siehe Kommentar an
#      der Stelle im Skript - hat live einmal patches/, docs/,
#      standalone-check/ etc. aus dem Arbeitsverzeichnis verschwinden
#      lassen, weil die nur auf unserem Branch existieren, nicht in
#      FreeCADs eigener Historie). HEAD bleibt dabei auf solver-sandbox,
#      kein detached HEAD.
#   5. git submodule update --init -- src/3rdParty/OndselSolver.
#   6. Alle Patches aus patches/PATCHES.txt in der dort angegebenen
#      Reihenfolge neu anwenden. Bricht bei JEDEM Patch, der nicht mehr
#      passt, sofort mit klarer Meldung ab (welcher Patch, warum) - kein
#      automatisches Ueberspringen/Ignorieren. Nachfolgende Patches werden
#      dann NICHT versucht (koennten vom fehlgeschlagenen abhaengen).
#   7. Zusammenfassung: was sich in src/Mod/Assembly geaendert hat, plus
#      Hinweis auf naechste Schritte (Build+Deploy, Kinematik-Tests).
#   8. Eigenen Build-Baum (SANDBOX_BUILD_DIR, falls vorhanden) komplett auf
#      denselben Zielcommit bringen - dort sind nur src/Mod/Assembly +
#      src/3rdParty/OndselSolver Symlinks hierher, der Rest von FreeCAD ist
#      ein eigener, sonst unbemerkt wegdriftender Checkout (siehe Kommentar
#      an der Stelle im Skript).
#   9. standalone-check/check-neon-sync.sh (rein lesend, siehe dortiger
#      Kommentar): prueft nebenbei, ob die eigene Neon-Qt6/PySide6/Shiboken6-
#      Umgebung noch dem aktuellen Stand des Neon-Repos entspricht - FreeCADs
#      eigenes CI-Skript pinnt dort selbst keine Version, "synchron mit
#      FreeCAD" heisst hier "synchron mit dem Neon-Repo". Kein automatisches
#      Update, nur ein Hinweis.
#
# Aufruf:
#   ./update-sandbox.sh                     # Zielcommit = aktueller HEAD von origin/main
#   ./update-sandbox.sh <commit-hash>       # expliziter Zielcommit
#   ./update-sandbox.sh --dry-run           # nur anzeigen, was passieren wuerde
#   ./update-sandbox.sh --dry-run <commit>  # Kombination (Reihenfolge der Argumente egal)
#   ./update-sandbox.sh --no-patches        # nur syncen, Patches NICHT automatisch anwenden

set -euo pipefail

SANDBOX_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Nur noch fuer Schritt 8/9 (Build-Baum-Mitsynchronisierung) gebraucht - NICHT mehr fuer den
# Zielcommit-Default (siehe Kommentar oben am Dateianfang, Nutzerauftrag 2026-10-03).
# src/Mod/Assembly + src/3rdParty/OndselSolver sind dort als Symlinks auf SANDBOX_DIR
# eingerichtet (automatisch synchron, kein eigener Sync-Schritt dafuer noetig) - der REST von
# FreeCAD dort (App-Kern, Sketcher, Gui usw.) ist ein EIGENER, nicht automatisch
# aktualisierter Checkout, der sonst unbemerkt vom Vanilla-Ziel wegdriftet (live gefunden:
# 943 Commits Rueckstand, 2026-09-27) - deshalb wird er hier explizit mitgezogen.
SANDBOX_BUILD_DIR="/home/maxx/freecad-sandbox/freecad-source"
PATCHES_DIR="${SANDBOX_DIR}/patches"
PATCHES_LIST="${PATCHES_DIR}/PATCHES.txt"

cd "$SANDBOX_DIR"

DRY_RUN=0
APPLY_PATCHES=1
TARGET_COMMIT=""
for arg in "$@"; do
  case "$arg" in
    --dry-run) DRY_RUN=1 ;;
    --no-patches) APPLY_PATCHES=0 ;;
    *) TARGET_COMMIT="$arg" ;;
  esac
done

log() { printf '\n=== %s -- %s ===\n\n' "$(date +%H:%M:%S)" "$*"; }
run() {
  if [[ $DRY_RUN -eq 1 ]]; then
    printf '[dry-run] %s\n' "$*"
  else
    "$@"
  fi
}

# Liest PATCHES.txt: Leerzeilen und #-Kommentare (auch am Zeilenende) ignorieren.
read_patch_list() {
  sed -e 's/#.*$//' -e 's/[[:space:]]*$//' "$PATCHES_LIST" | grep -v '^[[:space:]]*$' || true
}

log "Start. Aktueller Sandbox-Stand: $(git log -1 --format='%h %cd %s' --date=short)"

if [[ ! -f "$PATCHES_LIST" ]]; then
  echo "FEHLER: ${PATCHES_LIST} nicht gefunden." >&2
  exit 1
fi
mapfile -t PATCHES < <(read_patch_list)

log "Schritt 1/9: git fetch origin (+ Zielcommit-Default aus origin/main, falls keiner angegeben)"
# FCPROJECT-PATCH 2026-10-03: kein externes Verzeichnis mehr als Referenz - stattdessen
# immer frisch vom eigenen origin-Remote fetchen, dann ggf. daraus den Default ableiten.
run git fetch origin
if [[ -z "$TARGET_COMMIT" ]]; then
  # git rev-parse selbst ist rein lesend und laeuft deshalb auch im --dry-run echt mit.
  TARGET_COMMIT="$(git rev-parse origin/main)"
  echo "Kein Zielcommit angegeben - nehme aktuellen HEAD von origin/main: ${TARGET_COMMIT}"
fi

log "Schritt 2/9: von PATCHES.txt betroffene Dateien zuruecksetzen (damit sie den Sync nicht blockieren)"
# Dateiliste dynamisch aus den Patches selbst ableiten (jede "+++ b/<pfad>"-Zeile) statt
# manuell gepflegt - bleibt automatisch aktuell, wenn PATCHES.txt sich aendert.
PATCHED_FILES=()
if [[ ${#PATCHES[@]} -gt 0 ]]; then
  mapfile -t PATCHED_FILES < <(
    for p in "${PATCHES[@]}"; do
      grep -h '^+++ b/' "${PATCHES_DIR}/${p}" 2>/dev/null | sed 's#^+++ b/##'
    done | sort -u
  )
fi
for f in "${PATCHED_FILES[@]}"; do
  # Nur zuruecksetzen, was tatsaechlich hier ausgecheckt ist (z.B. src/App/* aus
  # freecad-app-getplacementof-partdesign-feature.patch ist es nicht).
  [[ -f "$f" ]] && run git checkout -- "$f"
done

log "Schritt 3/9: pruefe auf unerwartete lokale Aenderungen an getrackten Dateien"
KNOWN_DIRTY=("src/3rdParty/OndselSolver")
UNEXPECTED=""
while IFS= read -r line; do
  [[ -z "$line" ]] && continue
  is_known=0
  for f in "${KNOWN_DIRTY[@]}"; do
    [[ "$line" == *"$f" ]] && { is_known=1; break; }
  done
  [[ $is_known -eq 0 ]] && UNEXPECTED+="${line}"$'\n'
done < <(git status --porcelain --untracked-files=no)
if [[ -n "$UNEXPECTED" && $DRY_RUN -eq 0 ]]; then
  echo "FEHLER: unerwartete lokale Aenderungen an getrackten Dateien, breche ab:" >&2
  printf '%s' "$UNEXPECTED" >&2
  echo "(Erst manuell klaeren/sichern - z.B. git stash oder git checkout -- <Datei> - dann erneut starten.)" >&2
  exit 1
fi

BEFORE="$(git rev-parse HEAD)"
log "Schritt 4/9: src/Mod/Assembly + src/3rdParty/OndselSolver auf ${TARGET_COMMIT} bringen"
# WICHTIG (live als echter Vorfall aufgetreten, 2026-09-07): NIEMALS ein volles
# "git checkout <upstream-commit>" hier - das ersetzt den KOMPLETTEN Arbeitsbaum durch den
# Tree dieses reinen Upstream-Commits, der patches/, docs/, standalone-check/, resources/,
# .vscode/, SANDBOX_NOTES.md etc. gar nicht kennt (die existieren nur auf UNSEREM Branch,
# nicht in FreeCADs eigener Historie) - alle diese Ordner verschwinden dabei sofort aus dem
# Arbeitsverzeichnis (nichts geht in git verloren, der Branch-Pointer bleibt stehen, aber der
# Schreck ist trotzdem unnoetig). Pfad-beschraenkter Checkout stattdessen: aktualisiert NUR
# diese zwei Pfade auf den Stand von TARGET_COMMIT, HEAD bleibt auf dem eigenen Branch,
# nichts sonst wird angefasst.
run git checkout "$TARGET_COMMIT" -- src/Mod/Assembly src/3rdParty/OndselSolver

# Nutzerbefund 2026-09-27 (live zweimal reproduziert: "existiert bereits im Arbeitsverzeichnis"
# bei AssemblyIdentityGraph.cpp/h, CommandShowIdentityGraph.py, Assembly_ShowIdentityGraph.svg):
# der obige pfad-beschraenkte Checkout AKTUALISIERT nur Dateien, die im TARGET_COMMIT existieren -
# er LOESCHT NIE eine Datei, die bei uns (vorher committeter Stand) existiert, aber im
# TARGET_COMMIT fehlt (z.B. eine komplett neue Datei, die einer unserer Patches erst einfuehrt).
# Ohne diesen Schritt bleibt so eine Datei mit ihrem ALTEN (bereits gepatchten) Inhalt liegen und
# git apply schlaegt in Schritt 6/9 fehl, weil der Patch sie als "neue Datei" anlegen will, obwohl
# sie (aus reiner Checkout-Sicht) schon "existiert". Deshalb: jede Datei unter den beiden Pfaden,
# die im ALTEN committeten Stand (BEFORE) vorhanden war, aber im TARGET_COMMIT NICHT, explizit
# loeschen - NUR echtes 'rm', 'git checkout' kann das strukturell nicht.
mapfile -t OBSOLETE_FILES < <(
  comm -23 \
    <(git ls-tree -r --name-only "$BEFORE" -- src/Mod/Assembly src/3rdParty/OndselSolver | sort) \
    <(git ls-tree -r --name-only "$TARGET_COMMIT" -- src/Mod/Assembly src/3rdParty/OndselSolver | sort)
)
if [[ ${#OBSOLETE_FILES[@]} -gt 0 ]]; then
  log "Schritt 4b/9: ${#OBSOLETE_FILES[@]} Datei(en) loeschen, die im Ziel-Commit nicht mehr existieren"
  for f in "${OBSOLETE_FILES[@]}"; do
    echo "  loesche ${f}"
    [[ $DRY_RUN -eq 0 ]] && rm -f "$f"
  done
fi

log "Schritt 5/9: Submodule synchronisieren (OndselSolver)"
run git submodule update --init -- src/3rdParty/OndselSolver

if [[ $APPLY_PATCHES -eq 1 ]]; then
  log "Schritt 6/9: Patches aus PATCHES.txt neu anwenden (${#PATCHES[@]} Stueck)"
  for p in "${PATCHES[@]}"; do
    echo "--- ${p} ---"
    run git apply "${PATCHES_DIR}/${p}"
  done
else
  log "Schritt 6/9: uebersprungen (--no-patches)"
fi

if [[ $DRY_RUN -eq 0 ]]; then
  log "Schritt 7/9: Zusammenfassung src/Mod/Assembly (${BEFORE:0:10}..${TARGET_COMMIT:0:10})"
  git diff --stat "$BEFORE" "$TARGET_COMMIT" -- src/Mod/Assembly | tail -5
  echo
  echo "Naechste Schritte:"
  echo "  - Build+Deploy: cmake --build standalone-check/build --target build-and-deploy  (oder Strg+Shift+B)"
  echo "  - Kinematik-Test: cmake --build standalone-check/build --target kinematic-test-patched"

  log "Schritt 8/9: Build-Baum (${SANDBOX_BUILD_DIR}) komplett auf ${TARGET_COMMIT} bringen"
  if [[ ! -d "${SANDBOX_BUILD_DIR}/.git" ]]; then
    echo "Hinweis: ${SANDBOX_BUILD_DIR} nicht gefunden/kein Git-Repo - Schritt uebersprungen."
  else
    # Nutzerbefund 2026-09-27 ("wo sind die 940 Commits?"): src/Mod/Assembly und
    # src/3rdParty/OndselSolver sind in SANDBOX_BUILD_DIR als Symlinks auf DIESES Sandbox-Repo
    # eingerichtet (bleiben dadurch automatisch synchron) - der REST von FreeCAD dort wurde
    # bisher NIE aktualisiert. GEFAHR (ebenfalls live gefunden): ein normaler
    # "git checkout <commit>" wuerde versuchen, die getrackten Dateien UNTER genau diesen zwei
    # Pfaden wiederherzustellen - da dort aktuell Symlinks liegen (keine echten Verzeichnisse),
    # koennte das im schlimmsten Fall durch den Symlink hindurch in DIESES Sandbox-Repo
    # schreiben. Deshalb: Symlinks vor dem Checkout aushaengen (nur der Link, Zieldaten in
    # SANDBOX_DIR bleiben unberuehrt), danach die frisch wiederhergestellten (ungebrauchten)
    # echten Vanilla-Ordner an genau diesen Stellen wieder loeschen und die Symlinks neu anlegen.
    #
    # Nutzerauftrag "mit Abbruch-Absicherung": ALLES zwischen Aushaengen und Wiederherstellen
    # laeuft unter set -euo pipefail - jeder Fehler (Netzwerk, Konflikt) wuerde das Skript sonst
    # mit ausgehaengten Symlinks (kaputter Build-Baum) verlassen. Ein 'trap' auf EXIT stellt die
    # Symlinks deshalb UNBEDINGT wieder her, auch bei Abbruch/Ctrl-C/Fehler - idempotent, falls
    # sie schon korrekt stehen.
    SYMLINK_TARGETS=(
      "src/Mod/Assembly:${SANDBOX_DIR}/src/Mod/Assembly"
      "src/3rdParty/OndselSolver:${SANDBOX_DIR}/src/3rdParty/OndselSolver"
    )
    restore_build_symlinks() {
      cd "$SANDBOX_BUILD_DIR" || return
      for entry in "${SYMLINK_TARGETS[@]}"; do
        link_path="${entry%%:*}"
        link_target="${entry#*:}"
        if [[ -L "$link_path" ]]; then
          continue
        fi
        rm -rf "$link_path"
        ln -s "$link_target" "$link_path"
      done
    }
    trap restore_build_symlinks EXIT

    cd "$SANDBOX_BUILD_DIR"

    # Sicherheitscheck analog Schritt 3/9: unerwartete lokale Aenderungen AUSSERHALB der
    # Symlink-Pfade muessen vorher gesichert/committet sein (siehe
    # patches/2026.09.27-freecad-gui-qt6-qbytearray-qstring-fixes.patch als Beispiel, wie das
    # aussieht) - kein stillschweigendes Ueberschreiben.
    UNEXPECTED_BUILD_CHANGES="$(git status --porcelain --untracked-files=no -- . ':!src/Mod/Assembly' ':!src/3rdParty/OndselSolver' 2>/dev/null || true)"
    if [[ -n "$UNEXPECTED_BUILD_CHANGES" ]]; then
      echo "FEHLER: unerwartete lokale Aenderungen in ${SANDBOX_BUILD_DIR} (ausserhalb der" >&2
      echo "Symlink-Pfade) - erst sichern (z.B. als eigener Patch, siehe PATCHES.txt-Beispiele)" >&2
      echo "oder committen, dann erneut starten:" >&2
      echo "$UNEXPECTED_BUILD_CHANGES" >&2
      cd "$SANDBOX_DIR"
      exit 1
    fi

    for entry in "${SYMLINK_TARGETS[@]}"; do
      link_path="${entry%%:*}"
      [[ -L "$link_path" ]] && rm "$link_path"
    done

    git fetch origin
    git checkout "$TARGET_COMMIT"
    # Alle Submodule inkl. OndselSolver initialisieren - harmlos, auch fuer OndselSolver: an
    # dieser Stelle liegt dort noch der frisch ausgecheckte, ungebrauchte echte Vanilla-Ordner
    # (Symlink noch nicht wiederhergestellt), der gleich sowieso durch restore_build_symlinks()
    # geloescht und ersetzt wird.
    git submodule update --init --recursive

    # restore_build_symlinks() (per trap) laeuft gleich sowieso, hier zusaetzlich explizit VOR
    # der Rueckkehr nach SANDBOX_DIR, damit der Build-Baum sofort in konsistentem Zustand ist.
    restore_build_symlinks
    trap - EXIT

    cd "$SANDBOX_DIR"
    echo "Build-Baum jetzt bei $(git -C "$SANDBOX_BUILD_DIR" rev-parse --short HEAD) - Symlinks wiederhergestellt."
  fi

  log "Schritt 9/9: pruefe Neon-Qt6/PySide6/Shiboken6-Umgebung auf Versionsdrift (rein lesend)"
  "${SANDBOX_DIR}/standalone-check/check-neon-sync.sh" || true
else
  log "Dry-Run Ende - keine Aenderung vorgenommen."
fi
