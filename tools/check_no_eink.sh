#!/usr/bin/env bash
# check_no_eink - regression guard (parallels the no-em-dash hook).
#
# The 2.9" e-ink display (SSD1680) and its EC11 rotary knob were removed in lane
# N10 (v4.4). This gate keeps that hardware from creeping back into a user-facing
# surface (docs, the website, the embedded web UI, tools, issue templates, the
# top-level guides). It matches only the UNAMBIGUOUS hardware terms - the frozen
# scrModel NVS value is the string "eink", never these - so it needs no fragile
# allow-list for that value.
#
# Exempt: the changelog and historical ADR/proposal docs (they record the past
# decision), the NOTICE GxEPD2 attribution (the driver is still fetched via
# solide-drivers), the byte-frozen recorded fixture, and generated mirrors.
#
# Runs as a pre-commit hook over staged surface files, or standalone (no args)
# over the whole surface.
set -u

PAT='SSD1680|EC11|e-paper|e-Paper|rotary[ -]encoder|rotary[ -]knob'
EXEMPT='changelog|/adr/|/proposals/|docs_pack_data|website/docs/(guides|reference|api)/|support/fixtures|(^|/)NOTICE$|GxEPD|check_no_eink|check_tft_elf_no_eink|testing-tiers|workflows/checks.yml|tools/release_gate/|workflows/release.yml'
SURFACE_RE='^(docs/|website/|include/web/|tools/|\.github/|hardware/|README\.md|AGENTS\.md|CONTRIBUTING\.md)'

files=("$@")
if [ "${#files[@]}" -eq 0 ]; then
  # Standalone: scan the whole surface.
  while IFS= read -r f; do files+=("$f"); done < <(
    git ls-files docs website include tools .github hardware README.md AGENTS.md CONTRIBUTING.md
  )
fi

hits=""
for f in "${files[@]}"; do
  [ -f "$f" ] || continue
  printf '%s\n' "$f" | grep -qE "$SURFACE_RE" || continue
  printf '%s\n' "$f" | grep -qE "$EXEMPT" && continue
  m=$(grep -nHiE "$PAT" "$f" 2>/dev/null) && hits="$hits$m"$'\n'
done

if [ -n "$hits" ]; then
  printf '%s' "$hits"
  echo "e-ink / e-paper / EC11 rotary knob was removed in lane N10 and must not return"
  echo "to a user-facing surface. If this is legitimate history (changelog, ADR, proposal)"
  echo "or the GxEPD2 attribution, add it to EXEMPT in tools/check_no_eink.sh."
  exit 1
fi
exit 0
