// CUM-214 render-audit capture, HOSTED (Virtual Nimbus) variant. nimbusd sets
// window.NIMBUS_HOSTED and serves a container /api/state with no radio/AP/battery and
// no ESP OTA. Those honest-UI paths (CUM-218/CUM-279) only render under the hosted flag,
// so the device-only 95_audit_shots never saw them. This archives the hosted panes that
// differ (Home info line, Device connectivity + cloud + updates, the assistant subtabs)
// for the same eyeball pass. Output: screenshots/audit/hosted-<project>-<name>.png.
//   npx playwright test 96_audit_hosted
import { test, expect } from '@playwright/test';
import { seedToken, openApp, assertPane } from './_helpers.mjs';
import { ORCH } from '../fixtures.mjs';

const DIR = 'screenshots/audit';

async function shot(page, testInfo, name, locator) {
  const proj = testInfo.project.name;
  const target = locator || page;
  await target.screenshot({ path: `${DIR}/hosted-${proj}-${name}.png`, fullPage: !locator });
}

async function expandAll(page) {
  await page.evaluate(() => {
    document.querySelectorAll('details').forEach((d) => { d.open = true; });
    document.querySelectorAll('.hint.tip').forEach((h) => h.classList.add('open'));
    ['battsec', 'whatNext'].forEach((id) => { const e = document.getElementById(id); if (e) e.style.display = ''; });
  });
  await page.waitForTimeout(250);
}

// A realistic hosted /api/state + /api/health, matching 11_hosted.spec.mjs.
const HOSTED_STATE = {
  virtual: true, host: 'nimbusd', fw: 'v0', mode: 1, jobs: 0, running: true,
  needsOnboarding: false, provVerified: false, hasRing: false,
  heap: 44 * 1024 * 1024, heapTotal: 64 * 1024 * 1024,
  storeSD: true, storeLabel: 'instance disk', storeTotal: 0, storeFree: 0,
  batt: { valid: false, onExtPower: true },
  clock: { synced: true, epoch: Date.now() / 1000, local: '2026-09-01 00:00', tz: 'UTC' },
  profile: 1, effectiveProfile: 1, effectiveProfileName: 'Balanced',
};
const HOSTED_HEALTH = {
  ok: 3, degraded: 1, absent: 4,
  components: [
    { label: 'Engine', state: 'ok', detail: 'orchestrator running' },
    { label: 'Provider', state: 'ok', detail: 'at least one provider key configured' },
    { label: 'Memory', state: 'ok', detail: '0 memories' },
    { label: 'Storage', state: 'ok', detail: 'durable instance volume' },
    { label: 'Display', state: 'absent', detail: 'no screen on a hosted instance' },
    { label: 'Ring', state: 'absent', detail: 'no LED ring on a hosted instance' },
    { label: 'Audio', state: 'absent', detail: 'no mic or speaker on a hosted instance' },
    { label: 'Battery', state: 'absent', detail: 'on external power (hosted)' },
  ],
};

test.beforeEach(async ({ page }) => {
  await page.addInitScript(() => { window.NIMBUS_HOSTED = true; });
  await page.route('**/api/state', (r) => r.fulfill({ contentType: 'application/json', body: JSON.stringify(HOSTED_STATE) }));
  await page.route('**/api/health', (r) => r.fulfill({ contentType: 'application/json', body: JSON.stringify(HOSTED_HEALTH) }));
  // A hosted instance with 0 jobs has an empty sessions table; keep /api/orch consistent
  // with the hosted state so the capture does not show phantom device-mode sessions.
  await page.route('**/api/orch', (r) => r.fulfill({
    contentType: 'application/json', body: JSON.stringify({ ...ORCH, running: true, jobs: [] }),
  }));
});

for (const dest of ['home', 'memory', 'device']) {
  test(`hosted top ${dest}`, async ({ page }, testInfo) => {
    await seedToken(page);
    await openApp(page);
    await page.locator(`.tab[data-p=${dest}]`).click();
    await assertPane(page, dest);
    await page.waitForTimeout(400);
    if (dest === 'device') await expandAll(page);
    await shot(page, testInfo, `top-${dest}`);
  });
}

const SUBTABS = ['llm', 'connectors', 'tools', 'skills', 'routines', 'usage', 'safety'];
for (const sp of SUBTABS) {
  test(`hosted assistant ${sp}`, async ({ page }, testInfo) => {
    await seedToken(page);
    await openApp(page);
    await page.locator('.tab[data-p=assistant]').click();
    await assertPane(page, 'assistant');
    await page.locator(`.subtab[data-sp=${sp}]`).click();
    await expect(page.locator(`#subpane-${sp}`)).toBeVisible();
    await expandAll(page);
    await shot(page, testInfo, `assistant-${sp}`);
  });
}
