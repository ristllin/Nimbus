// Render fidelity - the fixture-drift / undefined-leak guard (CUM-214).
//
// The CUM-214 audit found the harness fixtures had drifted from the device /api
// contracts across many endpoints: a field the page read under a name the fixture
// never sent surfaced as the literal text "undefined" (Home "sta undefined", the
// Usage budget row "undefined", the Memory badge "undefined scratch"), a blank
// <select> (Downloads policy, the custom-endpoint wire), a chart that never drew,
// and a false "No SD card" banner over a present card. Each was a rendered LIE the
// screenshot audit would either chase as a phantom bug or, worse, let mask a real
// one. None of these throw, so the robustness suite (10_robustness) stayed green.
//
// This spec encodes the CLASS, not the instances: under the DEFAULT fixtures every
// pane and subtab must render with (1) no visible "undefined" / "NaN" / ": null"
// text, (2) every always-populated <select> holding a real option value, and (3) no
// "No SD card" claim while the fixtures report a card present. A new field-name drift
// on any endpoint fails here instead of shipping a lie under a green snapshot.
//
// It reads only the rendered DOM, so it also runs unchanged against a real device
// over LAN (TARGET=device) - there it asserts the device's own contract stays honest.
import { test, expect } from '@playwright/test';
import { seedToken, openApp, assertPane } from './_helpers.mjs';

const TOP = ['home', 'chat', 'memory', 'assistant', 'device'];
const SUBTABS = ['llm', 'connectors', 'tools', 'skills', 'routines', 'usage', 'safety'];

// Selects that a configured device always populates - none may be left blank, which is
// the "value set to a token no <option> carries" symptom (fetchPol name vs numeric
// option, an empty cust.conv). id -> the pane/subtab it lives on.
const MUST_FILL_SELECTS = {
  fetchpol: { sub: 'safety' },
  custConv: { sub: 'llm' },
  sttProv: { sub: 'llm' },
  ttsProv: { sub: 'llm' },
  capProbe: { sub: 'tools' },
  tlsSlots: { sub: 'tools' },
  emb_provider: { pane: 'memory' },
  emb_model: { pane: 'memory' },
};

// Reveal every collapsed group + conditional section so hidden dynamic text is checked
// too (a drifted field inside a closed <details> is still a lie waiting to render).
async function expandAll(page) {
  await page.evaluate(() => {
    document.querySelectorAll('details').forEach((d) => { d.open = true; });
    ['battsec', 'whatNext'].forEach((id) => { const e = document.getElementById(id); if (e) e.style.display = ''; });
  });
  await page.waitForTimeout(200);
}

// The rendered-lie assertion over the visible text of the whole app shell. innerText
// returns only what is actually painted (display:none is excluded), so this is exactly
// what a user - or an auditor eyeballing the screenshot - would read.
async function expectNoRenderedLies(page, where) {
  const txt = await page.locator('body').innerText();
  expect(txt, `${where}: a field rendered as literal "undefined"`).not.toMatch(/\bundefined\b/);
  expect(txt, `${where}: a number rendered as "NaN"`).not.toMatch(/\bNaN\b/);
  expect(txt, `${where}: a field rendered as ": null"`).not.toMatch(/:\s*null\b/);
}

test.describe('render fidelity (device mode, default fixtures)', () => {
  for (const dest of TOP) {
    test(`no rendered lies on ${dest}`, async ({ page }) => {
      await seedToken(page);
      await openApp(page);
      await page.locator(`.tab[data-p=${dest}]`).click();
      await assertPane(page, dest);
      await expandAll(page);
      await expectNoRenderedLies(page, dest);
    });
  }

  for (const sp of SUBTABS) {
    test(`no rendered lies on assistant/${sp}`, async ({ page }) => {
      await seedToken(page);
      await openApp(page);
      await page.locator('.tab[data-p=assistant]').click();
      await assertPane(page, 'assistant');
      await page.locator(`.subtab[data-sp=${sp}]`).click();
      await expect(page.locator(`#subpane-${sp}`)).toBeVisible();
      await expandAll(page);
      await expectNoRenderedLies(page, `assistant/${sp}`);
    });
  }

  test('every always-populated select holds a real option value', async ({ page }) => {
    await seedToken(page);
    await openApp(page);
    for (const [id, loc] of Object.entries(MUST_FILL_SELECTS)) {
      if (loc.pane) {
        await page.locator(`.tab[data-p=${loc.pane}]`).click();
        await assertPane(page, loc.pane);
      } else {
        await page.locator('.tab[data-p=assistant]').click();
        await assertPane(page, 'assistant');
        await page.locator(`.subtab[data-sp=${loc.sub}]`).click();
        await expect(page.locator(`#subpane-${loc.sub}`)).toBeVisible();
      }
      await expandAll(page);
      const sel = page.locator(`#${id}`);
      // The value is non-empty AND matches one of the select's own options - the exact
      // failure when applyOrch set the value to a token no <option> carries (blank box).
      const { value, options } = await sel.evaluate((el) => ({
        value: el.value,
        options: Array.from(el.options).map((o) => o.value),
      }));
      expect(value, `#${id} rendered blank (no option selected)`).not.toBe('');
      expect(options, `#${id} value "${value}" matches no option`).toContain(value);
    }
  });

  test('Memory does not claim "No SD card" while a card is present', async ({ page }) => {
    await seedToken(page);
    await openApp(page);
    await page.locator('.tab[data-p=memory]').click();
    await assertPane(page, 'memory');
    await expandAll(page);
    // The default fixtures report an SD card (mem/stats sdPresent, state storeSD); the
    // storage-tier banner must therefore be hidden, never the "No SD card" degraded line.
    await expect(page.locator('#tierbanner')).toBeHidden();
    await expect(page.locator('#pane-mem')).not.toContainText('No SD card');
  });
});

// Hosted (Virtual Nimbus) mode: nimbusd sets window.NIMBUS_HOSTED and serves a container
// /api/state with no radio/AP fields. The same class of lie bit hosted first ("ap
// undefined - undefined", CUM-279); this leg proves the honest hosted page carries no
// rendered lie either. Mirrors the hosted fixture in 11_hosted.spec.mjs.
const HOSTED_STATE = {
  virtual: true, host: 'nimbusd', fw: 'v0', mode: 1, jobs: 0, running: true,
  needsOnboarding: false, provVerified: false, hasRing: false,
  heap: 44 * 1024 * 1024, heapTotal: 64 * 1024 * 1024,
  storeSD: true, storeLabel: 'instance disk', storeTotal: 0, storeFree: 0,
  batt: { valid: false, onExtPower: true },
  clock: { synced: true, epoch: Date.now() / 1000, local: '2026-09-01 00:00', tz: 'UTC' },
  profile: 1, effectiveProfile: 1, effectiveProfileName: 'Balanced',
};

test.describe('render fidelity (hosted mode)', () => {
  test.beforeEach(async ({ page }) => {
    await page.addInitScript(() => { window.NIMBUS_HOSTED = true; });
    await page.route('**/api/state', (r) => r.fulfill({
      contentType: 'application/json', body: JSON.stringify(HOSTED_STATE),
    }));
  });

  for (const dest of ['home', 'memory', 'assistant', 'device']) {
    test(`no rendered lies on hosted ${dest}`, async ({ page }) => {
      await seedToken(page);
      await openApp(page);
      await page.locator(`.tab[data-p=${dest}]`).click();
      await assertPane(page, dest);
      await expandAll(page);
      await expectNoRenderedLies(page, `hosted ${dest}`);
    });
  }
});
