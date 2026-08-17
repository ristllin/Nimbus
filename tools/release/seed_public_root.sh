#!/usr/bin/env bash
# seed_public_root.sh - build + verify the tree that becomes the public repo's
# initial commit. Re-runnable; run a REHEARSAL anytime, the real cut at launch.
#
# Maintainer gate - needs the private ops/leak_patterns.txt (gitignored); it
# refuses to run without it, so it is a maintainer-only tool by design.
#
# What it does:
#   1. Exports the current HEAD via `git archive` into a clean staging dir
#      (never a file copy - build artifacts and ignored files can't leak in).
#   2. Runs the leak gate over the staging dir:
#        a. gitleaks (entropy/pattern scan, with the repo's .gitleaks.toml)
#        b. the Nimbus known-pattern grep list below (project-specific
#           identifiers generic scanners can't know)
#   3. On a clean gate, initializes the fresh single-commit root with a neutral
#      author identity, ready to push to the new public repo.
#
# Usage:
#   tools/release/seed_public_root.sh rehearse   # gate only, no repo created
#   tools/release/seed_public_root.sh cut vX.Y.Z # gate + fresh-root commit + tag
#
# The known-pattern list is THE contract: anything added to the private bench
# (new board tokens, new identifiers) gets a line here in the same change.
set -euo pipefail

MODE="${1:?usage: seed_public_root.sh <rehearse|cut> [version-tag]}"
VER="${2:-}"
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
STAGE="$(mktemp -d /tmp/nimbus-public-root.XXXXXX)"
trap 'rm -rf "$STAGE"' EXIT

echo "== staging HEAD via git archive -> $STAGE"
git -C "$REPO" archive HEAD | tar -x -C "$STAGE"

# ---- gate b: project-specific known patterns --------------------------------
# The pattern list is PRIVATE (it names the very identifiers being scrubbed) and
# lives in gitignored ops/leak_patterns.txt - one `<grep -E pattern>|<why>` per
# line. The gate refuses to run without it.
PATTERNS_FILE="$REPO/ops/leak_patterns.txt"
[ -f "$PATTERNS_FILE" ] || { echo "missing $PATTERNS_FILE - the gate cannot run"; exit 1; }

fail=0
echo "== gate: known-pattern greps ($(grep -cve '^\s*#' -e '^\s*$' "$PATTERNS_FILE")) patterns"
# Enumerate binary files ONCE (they are skipped by -I in the text scan below, so
# a token baked into a golden .bin/.png would slip past). Concatenate their
# extracted strings into one blob and scan it per pattern - far faster than
# re-walking the tree for every pattern.
binblob="$(mktemp)"; trap 'rm -rf "$STAGE" "$binblob"' EXIT
while IFS= read -r bin; do
  if ! grep -Iq . "$bin" 2>/dev/null; then
    printf '\n== %s ==\n' "$bin"
    strings -n 6 "$bin" 2>/dev/null || true
  fi
done < <(find "$STAGE" -type f -size +0c 2>/dev/null) > "$binblob"
while IFS= read -r entry; do
  case "$entry" in ''|'#'*) continue;; esac
  # Split on the LAST '|' so an ERE pattern may itself contain alternations.
  why="${entry##*|}"; pat="${entry%|*}"
  # -i: case-insensitive (a leaked SSID/name can differ only in case and still
  #   identify the owner). Text files. Disable errexit around grep: a no-match
  #   (rc 1) is the COMMON case and must not abort the gate under `set -e`.
  set +e
  hits=$(grep -rIniE "$pat" "$STAGE" 2>/dev/null); rc=$?
  set -e
  if [ "$rc" -eq 2 ]; then
    echo "GATE ERROR: pattern [$pat] is invalid (grep exit 2) - cannot trust the scan"; exit 1
  fi
  if [ "$rc" -eq 0 ]; then
    echo "LEAK [$pat] ($why):"
    echo "$hits" | head -10
    fail=1
  fi
  # Same pattern over the pre-extracted binary strings.
  if grep -qiE "$pat" "$binblob" 2>/dev/null; then
    echo "LEAK [$pat] ($why) in a BINARY file (see == markers in the strings blob)"
    grep -niE "$pat" "$binblob" 2>/dev/null | head -3
    fail=1
  fi
done < "$PATTERNS_FILE"

echo "== gate: gitleaks"
if command -v gitleaks >/dev/null; then
  CFG=()
  [ -f "$REPO/.gitleaks.toml" ] && CFG=(--config "$REPO/.gitleaks.toml")
  gitleaks detect --no-git --source "$STAGE" "${CFG[@]}" --redact -v || fail=1
else
  echo "gitleaks not installed - REFUSING to pass the gate without it"; fail=1
fi

if [ "$fail" -ne 0 ]; then
  echo "GATE FAILED - the staged tree is NOT publishable."; exit 1
fi
echo "GATE CLEAN."

if [ "$MODE" = "cut" ]; then
  : "${VER:?cut mode needs a version tag, e.g. v4.2.0}"
  echo "== cutting fresh root ($VER)"
  git -C "$STAGE" init -q -b main
  git -C "$STAGE" add -A
  GIT_AUTHOR_NAME="Roy Darnell" \
  GIT_AUTHOR_EMAIL="32160010+ristllin@users.noreply.github.com" \
  GIT_COMMITTER_NAME="Roy Darnell" \
  GIT_COMMITTER_EMAIL="32160010+ristllin@users.noreply.github.com" \
  git -C "$STAGE" commit -q -m "Initial public release ($VER)

Nimbus began as a private project; this public tree starts from its ${VER}
state. The prior development history (~850 commits, v2.x through ${VER})
remains in a private archive; docs/ release notes summarize the lineage."
  git -C "$STAGE" tag "$VER"
  # Drop the fresh root OUTSIDE the private repo tree. Anchor on the MAIN
  # checkout (parent of the shared .git), not the worktree toplevel - a
  # worktree lives under .claude/worktrees/, so its own parent would still
  # nest the root inside the private repo. Overridable with OUT_DIR.
  common="$(git -C "$REPO" rev-parse --path-format=absolute --git-common-dir)"
  mainrepo="$(dirname "$common")"
  OUT="${OUT_DIR:-$(dirname "$mainrepo")/nimbus-public-root}"
  case "$OUT" in
    "$mainrepo"/*) echo "refusing to write the public root inside the repo ($OUT)"; exit 1;;
  esac
  rm -rf "$OUT"
  mv "$STAGE" "$OUT"
  rm -f "$binblob"
  trap - EXIT
  echo "fresh root ready at $OUT - push with:"
  echo "  git -C $OUT remote add origin https://github.com/ristllin/Nimbus.git"
  echo "  git -C $OUT push -u origin main --tags"
fi
