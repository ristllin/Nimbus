// CUM-68: updates + storage surfaces. Update card consumes N5's result payload
// (result states + battery-gate messaging); files card shows card size/free +
// quota with a caption; danger zone has typed confirms.
import { test, expect } from '@playwright/test';
import { seedToken, openApp } from './_helpers.mjs';
import { STATE } from '../fixtures.mjs';

async function openUpdate(page) {
  await openApp(page);
  await page.locator('.tab[data-p=device]').click();
  await page.locator('#pane-set details summary', { hasText: 'Software update' }).click();
  await expect(page.locator('#fwCheck')).toBeVisible();
}

test('Check for updates shows an "available" result state', async ({ page }) => {
  await seedToken(page);
  await page.route('**/api/ota/check', (r) => r.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ result: 'available', installed: 'v4.3.0-pre', latest: 'v4.3.0' }) }));
  await openUpdate(page);
  await page.locator('#fwCheck').click();
  await expect(page.locator('#fwMsg')).toHaveAttribute('data-fb', 'ok');
  await expect(page.locator('#fwMsg')).toContainText('Update available');
});

test('Check for updates shows "up to date" when current (nothing-found state)', async ({ page }) => {
  await seedToken(page);
  await page.route('**/api/ota/check', (r) => r.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ result: 'current' }) }));
  await openUpdate(page);
  await page.locator('#fwCheck').click();
  await expect(page.locator('#fwMsg')).toHaveAttribute('data-fb', 'none');
  await expect(page.locator('#fwMsg')).toContainText('latest version');
});

test('Check for updates names the next step on error', async ({ page }) => {
  await seedToken(page);
  await page.route('**/api/ota/check', (r) => r.fulfill({ status: 500, body: 'x' }));
  await openUpdate(page);
  await page.locator('#fwCheck').click();
  await expect(page.locator('#fwMsg')).toHaveAttribute('data-fb', 'error');
  await expect(page.locator('#fwMsg')).toContainText('try again');
});

test('a logical error result (HTTP 200) renders an error, not a green Done', async ({ page }) => {
  await seedToken(page);
  await page.route('**/api/ota/check', (r) => r.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ result: 'error', msg: 'release server unreachable' }) }));
  await openUpdate(page);
  await page.locator('#fwCheck').click();
  await expect(page.locator('#fwMsg')).toHaveAttribute('data-fb', 'error');
  await expect(page.locator('#fwMsg')).toContainText('release server unreachable');
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
  // A wrong phrase must NOT post.
  page.once('dialog', (d) => d.accept('nope'));
  await page.locator('#sdReset').click();
  await page.waitForTimeout(200);
  expect(posted).toBe(false);
  // The exact phrase posts.
  page.once('dialog', (d) => d.accept('ERASE STORAGE'));
  await page.locator('#sdReset').click();
  await expect.poll(() => posted).toBe(true);
});
