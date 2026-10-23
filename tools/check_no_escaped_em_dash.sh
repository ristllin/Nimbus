#!/usr/bin/env bash
# check_no_escaped_em_dash - the em-dash lint's escaped-forms half (lane J, P2).
#
# The no-em-dash pre-commit hook catches the literal U+2014; firmware text used
# to carry it as escapes (\xE2\x80\x94, \u2014, &mdash;), which the hook never
# saw. This scans the same tree for the escaped forms. A line may opt out with
# an explicit reviewed marker - `em-dash-ok: <why>` - reserved for the parsers
# that must know the bytes (the no_dash filter, the rule classifier, this lint's
# own tests).
#
# Usage: tools/check_no_escaped_em_dash.sh [paths...]   (default: src lib include tools)
set -u
roots="${*:-src lib include tools}"
[ -n "$roots" ] || exit 0
hits=0
while IFS= read -r -d '' f; do
  while IFS=: read -r n line; do
    case "$line" in
      *em-dash-ok:*) continue ;;   # the reviewed marker (see the header)
    esac
    echo "$f:$n:$line"
    hits=$((hits + 1))
  done < <(grep -nE '\\x[eE]2\\x80\\x94|\\u2014|&#8212;|&#x2014;|&mdash;' "$f")
done < <(find $roots -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.c' -o -name '*.py' -o -name '*.mjs' \) -print0)
if [ "$hits" -gt 0 ]; then
  echo "escaped em dash forms are banned in firmware and tool text (AGENTS.md, Punctuation);"
  echo "write a real hyphen. A parser that must know the bytes marks the line em-dash-ok: <why>."
  exit 1
fi
exit 0
