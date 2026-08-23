# Web-app test harness (lane N1)

Host-side sim-e2e (T4) harness + Playwright suite for the embedded web app
(`include/web/ui_*.h`). It reconstructs the exact page the device serves - the
ordered concatenation of the PROGMEM fragments (`concat.mjs`, byte-identical to
`tools/webui_page.snapshot`) - serves it with mock `/api/*` responses
(`server.mjs` + `fixtures.mjs`), and drives it with Chromium.

The suite proves the web app renders and behaves without a device. Every spec is
written so it also runs unchanged against a real device over LAN (T5/HIL).

## Run (T4, host)

```bash
cd tools/webui_harness
npm install
npx playwright install chromium     # one-time
npm test                            # desktop + phone projects
```

Positive marker: Playwright prints `N passed`. The HTML report lands in
`playwright-report/` and screenshots in `screenshots/`.

## Run against a real device (T5/HIL)

The device must be reachable over LAN and you need a valid access token.

```bash
BASE_URL=http://<device-ip> TARGET=device NIMBUS_TOKEN=<token> npx playwright test
```

`TARGET=device` skips the local mock server (the device serves the real page and
real `/api`), and `NIMBUS_TOKEN` is seeded into `localStorage` so the sign-in
gate does not block. Mask the token in any pasted output. This tier needs bench
hardware (Freenove CYD or Nimbus-4) per the lane device-lock protocol; it is not
run in headless CI.

## Files

- `concat.mjs`   - rebuild the page from `include/web_pages.h` fragment order.
- `fixtures.mjs` - default `/api/*` JSON (contracts from N2/N3/N5 are stubs here).
- `server.mjs`   - the mock server (`PORT`, default 8790).
- `tests/`       - the Playwright specs; `_helpers.mjs` has the shared setup.
- `run_qa.sh`    - the lane QA entry point (concat check + full suite).
