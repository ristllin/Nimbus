// CUM-68: updates + storage surfaces. Update card consumes N5's result payload
// (result states + battery-gate messaging); files card shows card size/free +
// quota with a caption; danger zone has typed confirms.
import { test, expect } from '@playwright/test';
import { seedToken, openApp, confirmModal, dismissModal } from './_helpers.mjs';
import { STATE } from '../fixtures.mjs';

async function openUpdate(page) {
  await openApp(page);
  await page.locator('.tab[data-p=device]').click();
  await page.locator('#pane-set details summary', { hasText: 'Software update' }).click();
  await expect(page.locator('#fwCheck')).toBeVisible();
}

// The real device contract (CUM-249): POST /api/ota/check only ACCEPTS the check
// (202 {ok:true}); the check runs async and the verdict is read afterwards by
// polling /api/state's definitive `otaResult`. These stub `/api/state` to the
// settled outcome so the verdict is single-sourced from the same field the panel
// shows and can never disagree with it. `stateOf` overrides /api/state (used by
// both the initial load and the Check button's poll).
function stateOf(page, over) {
  return page.route('**/api/state', (r) => r.fulfill({ status: 200, contentType: 'application/json',
    body: JSON.stringify({ ...STATE, ...over }) }));
}
function acceptCheck(page) {
  return page.route('**/api/ota/check', (r) => r.fulfill({ status: 202, contentType: 'application/json', body: JSON.stringify({ ok: true }) }));
}

test('Check for updates shows an "available" result state (from otaResult)', async ({ page }) => {
  await seedToken(page);
  await acceptCheck(page);
  await stateOf(page, { ota: 'available', otaResult: 'new-version', otaLatest: 'v4.3.0' });
  await openUpdate(page);
  await page.locator('#fwCheck').click();
  await expect(page.locator('#fwMsg')).toHaveAttribute('data-fb', 'ok');
  await expect(page.locator('#fwMsg')).toContainText('Update available');
  await expect(page.locator('#fwMsg')).toContainText('v4.3.0');
});

test('Check for updates shows "up to date" when current (nothing-found state)', async ({ page }) => {
  await seedToken(page);
  await acceptCheck(page);
  await stateOf(page, { ota: 'up-to-date', otaResult: 'up-to-date' });
  await openUpdate(page);
  await page.locator('#fwCheck').click();
  await expect(page.locator('#fwMsg')).toHaveAttribute('data-fb', 'none');
  await expect(page.locator('#fwMsg')).toContainText('latest version');
});

// The CUM-249 regression guard: the engine reports AVAILABLE, so the Check verdict
// must NEVER read "latest version" - not even transiently while the async check is
// still settling. We record every #fwMsg text the button renders and assert
// "latest version" is never among them, and that it lands on "Update available".
test('never says "latest version" while the check settles to available (CUM-249)', async ({ page }) => {
  await seedToken(page);
  await acceptCheck(page);
  // First poll(s) are still 'pending'; then the check settles to 'new-version'.
  let polls = 0;
  await page.route('**/api/state', (r) => {
    const settled = polls++ >= 2;   // stay pending for the first two reads
    const over = settled
      ? { ota: 'available', otaResult: 'new-version', otaLatest: 'v4.3.0' }
      : { ota: 'checking', otaResult: 'pending', otaLatest: '' };
    return r.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ ...STATE, ...over }) });
  });
  await openUpdate(page);
  await page.evaluate(() => {
    window.__fwMsgs = [];
    const el = document.getElementById('fwMsg');
    new MutationObserver(() => window.__fwMsgs.push(el.textContent)).observe(el, { childList: true, characterData: true, subtree: true });
  });
  await page.locator('#fwCheck').click();
  await expect(page.locator('#fwMsg')).toHaveAttribute('data-fb', 'ok');
  await expect(page.locator('#fwMsg')).toContainText('Update available');
  const seen = await page.evaluate(() => window.__fwMsgs);
  expect(seen.some((t) => /latest version/i.test(t))).toBe(false);
});

test('a settled unreachable/failed result renders an error, not a green Done', async ({ page }) => {
  await seedToken(page);
  await acceptCheck(page);
  await stateOf(page, { ota: 'error', otaResult: 'unreachable', otaErr: 'no-release' });
  await openUpdate(page);
  await page.locator('#fwCheck').click();
  await expect(page.locator('#fwMsg')).toHaveAttribute('data-fb', 'error');
  await expect(page.locator('#fwMsg')).toContainText('release server');
});

test('a 409 local refusal names the next step (no Wi-Fi), not "check the network"', async ({ page }) => {
  await seedToken(page);
  await page.route('**/api/ota/check', (r) => r.fulfill({ status: 409, contentType: 'application/json',
    body: JSON.stringify({ ok: false, err: 'no-wifi', msg: "Can't check: no Wi-Fi. Connect and try again." }) }));
  await openUpdate(page);
  await page.locator('#fwCheck').click();
  await expect(page.locator('#fwMsg')).toHaveAttribute('data-fb', 'error');
  await expect(page.locator('#fwMsg')).toContainText('no Wi-Fi');
});

test('battery gate blocks install and shows the reason', async ({ page }) => {
  await seedToken(page);
  await page.route('**/api/state', (r) => r.fulfill({ status: 200, contentType: 'application/json',
    body: JSON.stringify({ ...STATE, ota: 'available', otaLatest: 'v4.3.0', otaBattOk: false, otaBattMsg: 'Charge to 40% to install (now 28%).' }) }));
  await openUpdate(page);
  await expect(page.locator('#fwBatt')).toBeVisible();
  await expect(page.locator('#fwBatt')).toContainText('Charge to 40%');
  await expect(page.locator('#fwInstall')).toBeDisabled();
});

test('files card shows real card size/free and the quota caption', async ({ page }) => {
  await seedToken(page);
  await openApp(page);
  await page.locator('.tab[data-p=memory]').click();
  await page.locator('#pane-mem details summary', { hasText: 'Files' }).click();
  await expect(page.locator('#filesQuota')).toContainText('Card:');
  await expect(page.locator('#filesQuota')).toContainText('free of');
  await expect(page.locator('#filesQuota')).toContainText('quota');
});

test('danger zone erase requires typing the exact phrase', async ({ page }) => {
  await seedToken(page);
  let posted = false;
  await page.route('**/api/sdreset', (r) => { posted = true; return r.fulfill({ status: 200, body: '{}' }); });
  await openApp(page);
  await page.locator('.tab[data-p=device]').click();
  await page.locator('#pane-set details summary', { hasText: 'Danger zone' }).click();
  // A wrong phrase must NOT post: the styled prompt (CUM-266) keeps OK disabled
  // until the exact word is typed, so filling a wrong word leaves nothing to click.
  await page.locator('#sdReset').click();
  await page.locator('#modalInput').fill('nope');
  await expect(page.locator('#modalOk')).toBeDisabled();
  await dismissModal(page);
  await page.waitForTimeout(200);
  expect(posted).toBe(false);
  // The exact phrase enables OK and posts.
  await page.locator('#sdReset').click();
  await confirmModal(page, 'ERASE STORAGE');
  await expect.poll(() => posted).toBe(true);
});
