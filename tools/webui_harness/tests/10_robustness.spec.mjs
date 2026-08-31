// Page-robustness floor (the white-screen class, AGENTS.md). Two bugs this encodes:
//
//   1. A JS SYNTAX ERROR or a thrown exception during render kills the single
//      concatenated <script>, so every pane stops switching while the static Home
//      markup still shows - a white screen under a highlighted tab. A merged head
//      shipped exactly this (an unclosed .catch() arrow in the CUM-270 restart poll);
//      the isolated audit-shot run stayed "green" because it never listened for page
//      errors and never asserted the target pane rendered. This suite listens.
//
//   2. A page that dies when one OPTIONAL state field is absent is the same class of
//      bug: a real device mid-upgrade (or a fixture that drifted behind a new state
//      field) can omit a key, and the page must degrade, not throw. So we drive the
//      whole app against deliberately sparse and then empty /api payloads and assert
//      the page raises no error. This doubles as the fixture-drift guard: the page may
//      never REQUIRE a field the mock does not serve.
//
// Every check here also runs unchanged against a real device (it only reads console +
// pageerror), so it is a T4/T5 floor, not a mock-only trick.
import { test, expect } from '@playwright/test';
import { seedToken, openApp, collectErrors } from './_helpers.mjs';

const TOP = ['home', 'chat', 'memory', 'assistant', 'device'];
const SUBTABS = ['llm', 'connectors', 'tools', 'skills', 'routines', 'usage', 'safety'];

// Walk every top destination and every Assistant subtab, giving each render a beat.
async function walkEverything(page) {
  for (const dest of TOP) {
    await page.locator(`.tab[data-p=${dest}]`).click();
    await page.waitForTimeout(150);
  }
  await page.locator('.tab[data-p=assistant]').click();
  for (const sp of SUBTABS) {
    await page.locator(`.subtab[data-sp=${sp}]`).click();
    await page.waitForTimeout(120);
  }
}

// The concatenated page parses and runs with no thrown error - this is the check that
// fails on a syntax error in any fragment (the exact regression the merged head had).
test('the whole page loads and switches every pane with no thrown errors', async ({ page }) => {
  const { errors } = collectErrors(page);
  await seedToken(page);
  await openApp(page);
  await walkEverything(page);
  await page.waitForTimeout(400);
  expect(errors, `page threw while rendering:\n${errors.join('\n')}`).toEqual([]);
});

// Sparse state: only the few fields the very first render needs; batt, cloud, card,
// ota*, psram, mdns and the rest are absent - as a device mid-upgrade can send them.
test('a sparse /api/state renders every pane without throwing', async ({ page }) => {
  const { errors } = collectErrors(page);
  await page.route('**/api/state', (r) => r.fulfill({ contentType: 'application/json',
    body: JSON.stringify({ fw: 'v0', mode: 1, running: true }) }));
  await page.route('**/api/orch', (r) => r.fulfill({ contentType: 'application/json',
    body: JSON.stringify({ running: true }) }));
  await seedToken(page);
  await openApp(page);
  await walkEverything(page);
  await page.waitForTimeout(400);
  expect(errors, `a missing optional field threw (white-screen class):\n${errors.join('\n')}`).toEqual([]);
});

// The extreme: every GET /api/* returns {} and every write returns {ok:true}. The page
// must still render every surface. If a future field becomes load-bearing, this fails
// here - forcing a guard (or an explicit decision) rather than a field silently
// required and a fixture silently drifting to match it.
test('every /api endpoint returning an empty object still renders every pane', async ({ page }) => {
  const { errors } = collectErrors(page);
  await page.route('**/api/**', (r) => {
    if (r.request().method() !== 'GET') return r.fulfill({ contentType: 'application/json', body: '{"ok":true}' });
    return r.fulfill({ contentType: 'application/json', body: '{}' });
  });
  await seedToken(page);
  await openApp(page);
  await walkEverything(page);
  await page.waitForTimeout(400);
  expect(errors, `an empty /api payload threw:\n${errors.join('\n')}`).toEqual([]);
});
