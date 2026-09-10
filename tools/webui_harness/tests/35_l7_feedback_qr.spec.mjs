// L7 (CUM-384/385/386): the sign-in code path on the auto-opened connect screen,
// action-button feedback for controls that used to fire silently, and the battery
// config section staying reachable on a board with no valid reading.
import { test, expect } from '@playwright/test';
import { seedToken, openApp } from './_helpers.mjs';
import { STATE } from '../fixtures.mjs';

// Battery object with the pack-config fields the device always emits (webui.cpp),
// parameterised by whether a reading is valid and whether monitoring is on.
function battFixture({ valid, battMon, divFixed = true }) {
  return {
    valid, battMon, divFixed, rawPackMv: valid ? 7400 : 0, senseMissing: false,
    percent: valid ? 74 : 0, millivolts: valid ? 7400 : 0, mvTrue: valid ? 7400 : 0,
    minsToEmpty: valid ? 180 : -1, onExtPower: false, charging: false,
    sleepMv: 6000, wakeMv: 6500, sleepOvr: false, brightOvr: false,
    capMah: 3500, chem: 'liion', cells: 0, curve: '', health: valid ? 96 : undefined,
    calibrated: false, settingsLive: valid,
  };
}
const stateWithBatt = (batt) => ({ ...STATE, batt });

async function openBattery(page) {
  await page.locator('nav.tabs .tab[data-p=device]').click();
  await expect(page.locator('#fwsec')).toBeVisible();       // device pane rendered
  await page.locator('#battsec > summary').click();          // expand the Battery group
}

// ---------- CUM-386: battery config reachable with no valid reading ----------

test('CUM-386: battery config is reachable on a freenove with no reading, monitoring off', async ({ page }) => {
  await seedToken(page);
  await page.route('**/api/state', (r) => r.fulfill({ json: stateWithBatt(battFixture({ valid: false, battMon: false })) }));
  await openApp(page);
  await openBattery(page);

  // The whole section no longer hides behind bt.valid.
  await expect(page.locator('#battsec')).toBeVisible();
  // Honest waiting copy, pointing at the monitor opt-in.
  await expect(page.locator('#battWait')).toBeVisible();
  await expect(page.locator('#battWait')).toContainText('Monitor the battery');
  // The pack config is populated from the always-emitted fields.
  await expect(page.locator('#battCapMah')).toHaveValue('3500');
  await expect(page.locator('#battChem')).toHaveValue('liion');
  // Fixed-divider board still hides the dead sense-resistor rows (CUM-370 preserved).
  await expect(page.locator('#battRtopRow')).toBeHidden();
  await expect(page.locator('#battRbotRow')).toBeHidden();
  // Calibrate needs a reading, so it is disabled (not silently broken).
  await expect(page.locator('#battcalBtn')).toBeDisabled();
  // Live rows read honestly, not as a fake value.
  await expect(page.locator('#battpct')).toHaveText('-');
});

test('CUM-386: Save works before any reading (the wiring is no longer nested in Calibrate)', async ({ page }) => {
  await seedToken(page);
  await page.route('**/api/state', (r) => r.fulfill({ json: stateWithBatt(battFixture({ valid: false, battMon: true })) }));
  let cfgPost = null;
  await page.route('**/api/config', (r) => { cfgPost = r.request(); return r.fulfill({ json: { ok: true } }); });
  await openApp(page);
  await openBattery(page);

  // Monitoring-on variant says it is waiting for a reading.
  await expect(page.locator('#battWait')).toContainText('waiting for a reading');
  // Save posts to /api/config directly - no need to press Calibrate first.
  await page.locator('#protSave').click();
  await expect.poll(() => cfgPost).not.toBeNull();
  expect(cfgPost.method()).toBe('POST');
  await expect(page.locator('#toast')).toHaveText('Battery settings saved');
});

test('CUM-386: a valid reading still shows the live readout and enables Calibrate', async ({ page }) => {
  await seedToken(page);
  await page.route('**/api/state', (r) => r.fulfill({ json: stateWithBatt(battFixture({ valid: true, battMon: true })) }));
  await openApp(page);
  await openBattery(page);
  await expect(page.locator('#battsec')).toBeVisible();
  await expect(page.locator('#battWait')).toBeHidden();      // no waiting banner on the valid path
  await expect(page.locator('#battpct')).toHaveText('74%');
  await expect(page.locator('#battcalBtn')).toBeEnabled();
});

// ---------- CUM-384: the code path is a first-class control on the connect screen ----------

test('CUM-384: the auth gate shows a visible "enter the code" control, not a buried link', async ({ page }) => {
  await page.goto('/');                                       // no token -> the connect gate
  await expect(page.locator('#authgate')).toBeVisible();
  const show = page.locator('#authshow');
  await expect(show).toBeVisible();
  // It is a real button (has a border), not a bare underlined text link.
  const border = await show.evaluate((el) => getComputedStyle(el).borderStyle);
  expect(border).toBe('solid');
  // The reveal is self-contained: it names the Sign-in QR and the Show code affordance
  // on the device screen, so the code is reachable without a Settings menu hunt.
  await show.click();
  const code = page.locator('#authcode');
  await expect(code).toBeVisible();
  await expect(code).toContainText('Show code');
  await expect(code).toContainText('Sign-in QR');
  await expect(page.locator('#authtok')).toBeVisible();
});

// ---------- CUM-385: controls that used to fire silently now confirm ----------

test('CUM-385: Deduplicate memories confirms on success', async ({ page }) => {
  await seedToken(page);
  await page.route('**/api/mem/vector**', (r) => r.fulfill({ json: { ok: true } }));
  await openApp(page);
  await page.locator('nav.tabs .tab[data-p=memory]').click();
  await page.locator('#pane-mem details summary', { hasText: 'Long-term memory' }).click();
  await page.locator('#memdedupe').click();
  await expect(page.locator('#toast')).toHaveText('Duplicates removed');
});

test('CUM-385: Deduplicate surfaces a failure instead of swallowing it', async ({ page }) => {
  await seedToken(page);
  await page.route('**/api/mem/vector**', (r) => r.fulfill({ status: 500, body: 'err' }));
  await openApp(page);
  await page.locator('nav.tabs .tab[data-p=memory]').click();
  await page.locator('#pane-mem details summary', { hasText: 'Long-term memory' }).click();
  await page.locator('#memdedupe').click();
  await expect(page.locator('#toast')).toContainText('try again');
});

test('CUM-385: the Automatic updates toggle surfaces a save failure', async ({ page }) => {
  await seedToken(page);
  await page.route('**/api/config', (r) => r.fulfill({ status: 500, body: 'err' }));
  await openApp(page);
  await page.locator('nav.tabs .tab[data-p=device]').click();
  await page.locator('#fwsec > summary').click();
  await page.locator('#autoUpd').click();
  await expect(page.locator('#toast')).toContainText('try again');
});
