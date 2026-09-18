#!/usr/bin/env bash
#
# Put this repo's Apps where App Lab can find them.
#
# App Lab discovers Apps by a flat scan of ~/ArduinoApps: every App has to be a
# top-level directory there. This repo holds two of them plus one shared brick,
# so something has to link them into place. That is this script.
#
# What it does, per App:
#   1. copies bricks/nesso_companion into apps/<name>/bricks/   (the brick is
#      per-App by design in App Lab - there is no shared location - so the one
#      source here is copied rather than duplicated in git)
#   2. symlinks ~/ArduinoApps/<name> -> apps/<name>
#
# The symlink is deliberate: Docker resolves it when it bind-mounts the App, so
# inside the container /app is an ordinary directory. That means you edit the
# repo and the running App sees it - no copying back and forth.
#
# Re-run it whenever you change bricks/. It is idempotent.

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APPS_DIR="${ARDUINO_APPS_DIR:-$HOME/ArduinoApps}"
BRICK_SRC="$REPO/bricks/nesso_companion"

CHECK_ONLY=0
[[ "${1:-}" == "--check" ]] && CHECK_ONLY=1

say()  { printf '%s\n' "$*"; }
warn() { printf '  ! %s\n' "$*" >&2; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }

[[ -d "$BRICK_SRC" ]] || die "brick source missing: $BRICK_SRC"
[[ -d "$APPS_DIR" ]]  || die "$APPS_DIR does not exist - is this an App Lab board?"

apps=()
for dir in "$REPO"/apps/*/; do
  [[ -f "$dir/app.yaml" ]] && apps+=("$(basename "$dir")")
done
(( ${#apps[@]} )) || die "no Apps found under $REPO/apps"

status=0

for app in "${apps[@]}"; do
  src="$REPO/apps/$app"
  link="$APPS_DIR/$app"
  say "$app"

  # --- the brick copy -------------------------------------------------------
  dest="$src/bricks/nesso_companion"
  if (( CHECK_ONLY )); then
    if [[ ! -d "$dest" ]]; then
      warn "brick not installed"
      status=1
    elif ! diff -rq -x '__pycache__' "$BRICK_SRC" "$dest" >/dev/null 2>&1; then
      warn "brick copy differs from bricks/ - re-run install.sh"
      status=1
    else
      say "  brick up to date"
    fi
  else
    mkdir -p "$src/bricks"
    rm -rf "$dest"
    cp -r "$BRICK_SRC" "$dest"
    # The copy is generated; a stale __pycache__ from the source would ship with it.
    find "$dest" -name '__pycache__' -type d -prune -exec rm -rf {} + 2>/dev/null || true
    say "  brick copied from bricks/nesso_companion"
  fi

  # --- the symlink ----------------------------------------------------------
  if [[ -L "$link" ]]; then
    current="$(readlink -f "$link")"
    if [[ "$current" == "$src" ]]; then
      say "  linked"
    else
      if (( CHECK_ONLY )); then
        warn "linked to $current, expected $src"
        status=1
      else
        ln -sfn "$src" "$link"
        say "  relinked (was $current)"
      fi
    fi
  elif [[ -e "$link" ]]; then
    # A real directory is here. Never delete it - it may be someone's work.
    warn "$link is a real directory, not a link. Move or remove it, then re-run."
    status=1
  else
    if (( CHECK_ONLY )); then
      warn "not linked"
      status=1
    else
      ln -s "$src" "$link"
      say "  linked into $APPS_DIR"
    fi
  fi
done

say ""
if (( CHECK_ONLY )); then
  (( status == 0 )) && say "Everything is in place." || say "Run ./install.sh to fix the above."
else
  say "Done. App Lab should now list: ${apps[*]}"
  say "Start one with:  arduino-app-cli app start user:${apps[0]}"
fi

exit $status
