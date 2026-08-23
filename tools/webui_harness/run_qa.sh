#!/usr/bin/env bash
# run_qa.sh - lane N1 web-app QA entry point.
#   1. webui concat + snapshot invariants (T0/T3)
#   2. Playwright sim-e2e suite, desktop + phone (T4)
# Pass TARGET=device BASE_URL=http://<ip> NIMBUS_TOKEN=<tok> to run tier T5/HIL
# against a real device instead of the local mock server.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

echo "== [T0/T3] webui concat + snapshot =="
python3 "$ROOT/tools/webui_concat_check.py"

echo "== [T4] Playwright web-app suite =="
cd "$HERE"
if [ ! -d node_modules ]; then npm install; fi
npx playwright test "$@"
echo "== web-app QA OK =="
