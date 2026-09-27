// The setup-network recovery journey (CUM-452, CUM-453, CUM-454).
//
// - CUM-452: "Publish Setup Network" states what happens next. Over the home Wi-Fi the
//   page is about to lose the device, so the result line says the device screen now
//   shows the setup network and that this page will disconnect; on the setup network
//   itself the page stays, so it says joining is paused. A failure shows the error.
// - CUM-453: the sign-in gate's code field does not autocapitalize or autocorrect, and
//   a code typed with stray capitals or spaces is normalized before the exchange (codes
//   are minted as lowercase hex).
// - CUM-454: while the gate is up nothing of the app shows through: the sidebar/bottom
//   bar and the panes are hidden and the page does not scroll; a sign-in that resumes
//   without a reload restores all of it.
import { test, expect } from '@playwright/test';
import { seedToken, openApp, assertPane } from './_helpers.mjs';

async function toRecovery(page) {
  await seedToken(page);
  await openApp(page);
  await page.locator('.tab[data-p=device]').click();
  await assertPane(page, 'device');
  const group = page.locator('#pane-set > details.setgroup', {
    has: page.locator('> summary', { hasText: 'Connectivity' }),
  });
  if (!(await group.evaluate((el) => el.open))) await group.locator('> summary').click();
  const rec = page.locator('#wifiRecovery');
  if (!(await rec.evaluate((el) => el.open))) await rec.locator('> summary').click();
  await expect(page.locator('#wifiAp')).toBeVisible();
}

// Answer POST /api/wifi action=publishap with `body`, and count GET reloads of the list.
async function routePublish(page, status, body) {
  const seen = { publish: null, gets: 0 };
  await page.route('**/api/wifi', async (route) => {
    const req = route.request();
    if (req.method() === 'POST') {
      seen.publish = req.postData() || '';
      return route.fulfill({ status, contentType: 'application/json', body: JSON.stringify(body) });
    }
    seen.gets++;
    return route.fallback();
  });
  return seen;
}

async function publish(page) {
  await page.locator('#wifiAp').click();
  await expect(page.locator('#modalOv.show')).toBeVisible();
  // The confirm explains the consequence before anything happens.
  await expect(page.locator('#modalBody')).toContainText('screen');
  await page.locator('#modalOk').click();
}

test('CUM-452: publishing over the home Wi-Fi states the next step and keeps it on screen', async ({ page }) => {
  await toRecovery(page);
  const seen = await routePublish(page, 200, { ok: true, queued: true, apSsid: 'Nimbus-4-setup', onAp: false });
  await publish(page);
  const msg = page.locator('#wifiApMsg');
  await expect(msg).toContainText('The device screen now shows the setup network; this page will disconnect.');
  await expect(msg).toContainText('Nimbus-4-setup');
  expect(seen.publish).toContain('publishap');
  await expect(page.locator('#toast')).toHaveText('Setup network published');
  // The page is about to lose the device: no reload may overwrite the next step.
  const getsAfter = seen.gets;
  await page.waitForTimeout(2200);
  expect(seen.gets).toBe(getsAfter);
  await expect(msg).toContainText('this page will disconnect');
  await expect(page.locator('#wifiAp')).toBeEnabled();
});

test('CUM-452: publishing from the setup network itself does not claim a disconnect', async ({ page }) => {
  await toRecovery(page);
  await routePublish(page, 200, { ok: true, queued: true, apSsid: 'Nimbus-4-setup', onAp: true });
  await publish(page);
  const msg = page.locator('#wifiApMsg');
  await expect(msg).toContainText('The device screen now shows the setup network.');
  await expect(msg).not.toContainText('disconnect');
});

test('CUM-452: a failed publish shows the error and re-enables the button', async ({ page }) => {
  await toRecovery(page);
  await routePublish(page, 500, { error: "Couldn't publish the setup network. Try again." });
  await publish(page);
  await expect(page.locator('#wifiApMsg')).toHaveText("Couldn't publish the setup network. Try again.");
  await expect(page.locator('#wifiAp')).toBeEnabled();
});

test('CUM-452: the button shows a pending state while the request is in flight', async ({ page }) => {
  await toRecovery(page);
  let release;
  const gate = new Promise((r) => { release = r; });
  await page.route('**/api/wifi', async (route) => {
    if (route.request().method() !== 'POST') return route.fallback();
    await gate;
    return route.fulfill({ status: 200, contentType: 'application/json',
      body: JSON.stringify({ ok: true, queued: true, apSsid: 'Nimbus-4-setup', onAp: false }) });
  });
  await publish(page);
  await expect(page.locator('#wifiAp')).toBeDisabled();
  await expect(page.locator('#wifiApMsg')).toContainText('Publishing');
  release();
  await expect(page.locator('#wifiApMsg')).toContainText('this page will disconnect');
  await expect(page.locator('#wifiAp')).toBeEnabled();
});

test('CUM-453: the code field never autocapitalizes and a typed code is normalized', async ({ page }) => {
  let exchange = null;
  await page.route('**/api/signin/exchange', async (route) => {
    exchange = route.request().postData() || '';
    await route.fulfill({ status: 401, contentType: 'application/json', body: '{"error":"invalid or expired code"}' });
  });
  await page.goto('/');
  await expect(page.locator('#authgate')).toBeVisible();
  await page.locator('#authshow').click();
  const input = page.locator('#authtok');
  await expect(input).toHaveAttribute('autocapitalize', 'off');
  await expect(input).toHaveAttribute('autocorrect', 'off');
  await expect(input).toHaveAttribute('spellcheck', 'false');
  await expect(input).toHaveAttribute('autocomplete', 'off');
  // A phone keyboard capitalizes the first letter; people add spaces.
  await input.fill('  A1B2C3D4E5F6 ');
  await page.locator('#authuse').click();
  await expect.poll(() => exchange).not.toBeNull();
  expect(exchange).toContain('a1b2c3d4e5f6');
  expect(exchange).not.toContain('A1B2');
  // The gate points at where the code is shown on the device.
  await expect(page.locator('#authcode')).toContainText('Setup screen');
});

// Everything the gate is covering: the navigation shell and every pane.
async function appHidden(page) {
  return page.evaluate(() => {
    const vis = (el) => getComputedStyle(el).visibility;
    const side = document.querySelector('aside.side');
    const panes = [...document.querySelectorAll('.pane')];
    return {
      side: vis(side),
      panes: panes.map(vis),
      rootOverflow: getComputedStyle(document.documentElement).overflowY,
      bodyOverflow: getComputedStyle(document.body).overflowY,
      gateOverscroll: (() => {
        const g = document.getElementById('authgate');
        return g ? getComputedStyle(g).overscrollBehaviorY : null;
      })(),
    };
  });
}

test('CUM-454: while the gate is up the app is hidden and the page does not scroll', async ({ page }, testInfo) => {
  await page.goto('/');                                  // no token -> the gate
  await expect(page.locator('#authgate')).toBeVisible();
  const s = await appHidden(page);
  expect(s.side).toBe('hidden');
  expect(s.panes.length).toBeGreaterThan(3);
  for (const v of s.panes) expect(v).toBe('hidden');
  expect(s.rootOverflow).toBe('hidden');
  expect(s.bodyOverflow).toBe('hidden');
  expect(s.gateOverscroll).toBe('none');
  // The gate covers the whole viewport.
  const box = await page.locator('#authgate').boundingBox();
  const vp = page.viewportSize();
  expect(box.y).toBeLessThanOrEqual(0);
  expect(box.height).toBeGreaterThanOrEqual(vp.height);
  // A real scroll gesture moves nothing (desktop wheel; mobile emulation has no wheel).
  if (!testInfo.project.name.includes('phone')) {
    await page.mouse.move(vp.width / 2, vp.height / 2);
    await page.mouse.wheel(0, 2000);
    await page.waitForTimeout(200);
    expect(await page.evaluate(() => window.scrollY)).toBe(0);
  }
  // Nothing of the app can be clicked through or read through the gate.
  await expect(page.locator('#pane-dash')).toBeHidden();
  await expect(page.locator('nav.tabs')).toBeHidden();
});

test('CUM-454: a 401 that raises the gate over a signed-in page hides the app too', async ({ page }) => {
  await seedToken(page);
  await openApp(page);
  await page.route('**/api/state**', (r) => r.fulfill({ status: 401, contentType: 'application/json', body: '{"error":"x"}' }));
  await page.evaluate(() => fetch('/api/state'));
  await expect(page.locator('#authgate')).toBeVisible();
  const s = await appHidden(page);
  expect(s.side).toBe('hidden');
  for (const v of s.panes) expect(v).toBe('hidden');
  expect(s.rootOverflow).toBe('hidden');
});

test('CUM-454: signing in without a reload restores the app and scrolling', async ({ page }) => {
  // Storage blocked (private browsing): the exchange resumes in place, no reload.
  await page.addInitScript(() => { Storage.prototype.setItem = function () { throw new Error('blocked'); }; });
  await page.route('**/api/signin/exchange', (r) =>
    r.fulfill({ status: 200, contentType: 'application/json', body: '{"token":"MEMTOK1"}' }));
  await page.goto('/');
  await expect(page.locator('#authgate')).toBeVisible();
  await page.locator('#authshow').click();
  await page.locator('#authtok').fill('a1b2c3d4e5f6');
  await page.locator('#authuse').click();
  await expect(page.locator('#authgate')).toHaveCount(0);
  const s = await appHidden(page);
  expect(s.side).toBe('visible');
  expect(s.rootOverflow).not.toBe('hidden');
  expect(s.bodyOverflow).not.toBe('hidden');
  await expect(page.locator('nav.tabs')).toBeVisible();
  await assertPane(page, 'home');
});

test('CUM-454: signing in with a reload lands on the full app', async ({ page }) => {
  await page.route('**/api/signin/exchange', (r) =>
    r.fulfill({ status: 200, contentType: 'application/json', body: '{"token":"HARNESSTOKEN123456"}' }));
  await page.goto('/');
  await expect(page.locator('#authgate')).toBeVisible();
  await page.locator('#authshow').click();
  await page.locator('#authtok').fill('a1b2c3d4e5f6');
  await Promise.all([page.waitForEvent('load'), page.locator('#authuse').click()]);
  await expect(page.locator('#authgate')).toHaveCount(0);
  await expect(page.locator('nav.tabs')).toBeVisible();
  await assertPane(page, 'home');
  const s = await appHidden(page);
  expect(s.side).toBe('visible');
  expect(s.rootOverflow).not.toBe('hidden');
});
