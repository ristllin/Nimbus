// Hosted (Virtual Nimbus) honest-UI + pane-switch robustness (CUM-279).
//
// The live hosted page hit two things a device-only harness never did:
//   1. Device-only chrome shown faked or dead on a hosted instance (a "Free RAM
//      65536K of 65536K" tile, an "ap undefined - undefined" network line, dead
//      audio-test buttons, an ESP OTA card that a VN cannot use).
//   2. A tab click bouncing back to Home / navigation dying - a hosted-only state
//      path threw during a pane loader and took the whole nav down. The sparse-
//      payload robustness suite did NOT set the hosted flag, so it missed the live
//      trigger. These specs set window.NIMBUS_HOSTED (as nimbusd's bootstrap does).
//
// Every check reads console + pageerror or the visible DOM, so it also runs against
// a real nimbusd-served page over LAN (TARGET=device) unchanged.
import { test, expect } from '@playwright/test';
import { seedToken, openApp, collectErrors } from './_helpers.mjs';

// Mark the page hosted before any app script runs (nimbusd injects this in its page
// bootstrap). Must be called before openApp().
async function markHosted(page) {
  await page.addInitScript(() => { window.NIMBUS_HOSTED = true; });
}

// A realistic hosted /api/state: a real container heap (not the device cap), no AP /
// station / mDNS, Orchestrator mode, no ota* fields, no PSRAM, external power.
const HOSTED_STATE = {
  virtual: true, host: 'nimbusd', fw: 'v0', mode: 1, jobs: 0, running: true,
  needsOnboarding: false, provVerified: false, hasRing: false,
  heap: 44 * 1024 * 1024, heapTotal: 64 * 1024 * 1024,
  storeSD: true, storeLabel: 'instance disk', storeTotal: 0, storeFree: 0,
  batt: { valid: false, onExtPower: true },
  clock: { synced: true, epoch: Date.now() / 1000, local: '2026-09-01 00:00', tz: 'UTC' },
  profile: 1, effectiveProfile: 1, effectiveProfileName: 'Balanced',
};

function routeHosted(page, opts = {}) {
  return Promise.all([
    page.route('**/api/state', (r) => r.fulfill({ contentType: 'application/json',
      body: JSON.stringify(HOSTED_STATE) })),
    page.route('**/api/health', (r) => r.fulfill({ contentType: 'application/json',
      body: JSON.stringify({
        ok: 3, degraded: 1, absent: 4,
        components: [
          { label: 'Engine', state: 'ok', detail: 'orchestrator running' },
          { label: 'Provider', state: opts.providerOk ? 'ok' : 'degraded',
            detail: opts.providerOk ? 'at least one provider key configured'
                                    : 'no provider key - add one to reply' },
          { label: 'Memory', state: 'ok', detail: '0 memories' },
          { label: 'Storage', state: 'ok', detail: 'durable instance volume' },
          { label: 'Display', state: 'absent', detail: 'no screen on a hosted instance' },
          { label: 'Ring', state: 'absent', detail: 'no LED ring on a hosted instance' },
          { label: 'Audio', state: 'absent', detail: 'no mic or speaker on a hosted instance' },
          { label: 'Battery', state: 'absent', detail: 'on external power (hosted)' },
        ],
      }) })),
  ]);
}

// (1) Every pane switches with no thrown error under the HOSTED flag + payloads -
//     the live trigger the device-only robustness suite missed.
test('hosted: every pane switches with no thrown error', async ({ page }) => {
  const { errors } = collectErrors(page);
  await markHosted(page);
  await routeHosted(page);
  await seedToken(page);
  await openApp(page);
  for (const dest of ['home', 'chat', 'memory', 'assistant', 'device']) {
    await page.locator(`.tab[data-p=${dest}]`).click();
    await page.waitForTimeout(150);
    // Navigation actually completed: the tab is marked and Home is not stuck under it.
    await expect(page.locator(`.tab[data-p=${dest}]`)).toHaveClass(/on/);
  }
  await page.waitForTimeout(300);
  expect(errors, `hosted page threw during tab switch:\n${errors.join('\n')}`).toEqual([]);
});

// (2) The defensive guard directly: a pane loader that THROWS must not break
//     navigation. Fails on the pre-fix code (the throw escaped goDest's onclick).
test('hosted: a throwing pane loader does not break navigation', async ({ page }) => {
  const { errors } = collectErrors(page);
  await markHosted(page);
  await routeHosted(page);
  await seedToken(page);
  await openApp(page);
  // Force the Home loader to throw synchronously, the way a hosted-only absent field
  // could. loadPane('dash') calls loadHealth(); the guard must swallow it.
  await page.evaluate(() => { window.loadHealth = () => { throw new Error('boom'); }; });
  await page.locator('.tab[data-p=assistant]').click();
  await page.waitForTimeout(120);
  await page.locator('.tab[data-p=home]').click();
  await page.waitForTimeout(200);
  await expect(page.locator('#pane-dash')).toBeVisible();     // still switched
  await expect(page.locator('.tab[data-p=home]')).toHaveClass(/on/);
  expect(errors, `a throwing loader escaped navigation:\n${errors.join('\n')}`).toEqual([]);
});

// (3) Honest RAM tile: the container memory in MB, never the device "floor 34K" line.
test('hosted: the memory tile is honest (MB, no ESP heap floor)', async ({ page }) => {
  await markHosted(page);
  await routeHosted(page);
  await seedToken(page);
  await openApp(page);
  await page.waitForTimeout(300);
  const tiles = await page.locator('#devtiles').innerText();
  expect(tiles).toContain('MB');
  expect(tiles).not.toContain('floor 34K');
});

// (4) The network line ("ap undefined - undefined") is gone on a hosted instance.
test('hosted: no "ap undefined" network line', async ({ page }) => {
  await markHosted(page);
  await routeHosted(page);
  await seedToken(page);
  await openApp(page);
  await page.waitForTimeout(300);
  const info = await page.locator('#info').innerText();
  expect(info.toLowerCase()).not.toContain('ap ');
  expect(info).not.toContain('undefined');
});

// (5) Hardware controls hidden: the ESP OTA card and the audio probe buttons.
test('hosted: ESP OTA card and audio probe buttons are hidden', async ({ page }) => {
  await markHosted(page);
  await routeHosted(page);
  await seedToken(page);
  await openApp(page);
  await page.waitForTimeout(300);
  await expect(page.locator('#fwsec')).toBeHidden();        // ESP OTA card
  await expect(page.locator('#hpMic')).toHaveCount(0);       // no Mic Test button
  await expect(page.locator('#hpBeep')).toHaveCount(0);      // no Speaker Tone button
});

// (6) Provider-degraded resolve link (item 2): the Health row links to Providers.
test('hosted: a degraded provider row offers a resolve link', async ({ page }) => {
  await markHosted(page);
  await routeHosted(page, { providerOk: false });
  await seedToken(page);
  await openApp(page);
  await page.waitForTimeout(300);
  const fix = page.locator('#healthlist [data-fix=llm]');
  await expect(fix).toBeVisible();
  await fix.click();   // jumps to the Assistant > Models subtab
  await expect(page.locator('.subtab[data-sp=llm]')).toHaveClass(/on/);
});
