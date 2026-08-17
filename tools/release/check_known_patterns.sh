#!/usr/bin/env bash
# nimbus-known-patterns - pre-commit gate: grep STAGED files against the
# private known-leak list (ops/leak_patterns.txt, gitignored - bench tokens,
# identifiers, PII that must never reach a public tree).
#
# Public contributors do not have ops/leak_patterns.txt; the hook then WARNS
# and passes (exit 0) - the private list is a maintainer-side belt on top of
# gitleaks, not a requirement to contribute.
#
# Pattern file format: <grep -E pattern>|<why it must never appear>
# Lines starting with '#' and blank lines are ignored.
set -u

PATTERNS="ops/leak_patterns.txt"

if [ ! -f "$PATTERNS" ]; then
  echo "WARNING: $PATTERNS not present (private maintainer file) - known-pattern gate skipped." >&2
  exit 0
fi

fail=0
while IFS= read -r entry; do
  case "$entry" in ''|'#'*) continue ;; esac
  # Split on the LAST '|' so an ERE pattern may contain alternations.
  why="${entry##*|}"; pat="${entry%|*}"
  for f in "$@"; do
    # Check the STAGED content (index version), not the working tree.
    if git show ":$f" 2>/dev/null | grep -iEq -- "$pat"; then
      echo "LEAK PATTERN in staged $f: /$pat/ - ${why:-known-private pattern}" >&2
      fail=1
    fi
    # The path itself can leak too (e.g. a bench-identifier filename).
    if printf '%s' "$f" | grep -iEq -- "$pat"; then
      echo "LEAK PATTERN in filename $f: /$pat/ - ${why:-known-private pattern}" >&2
      fail=1
    fi
  done
done < "$PATTERNS"

exit "$fail"
