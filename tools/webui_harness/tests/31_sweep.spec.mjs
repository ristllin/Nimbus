// CUM-31 acceptance: a sweep proving no async action ends without a visible
// result state. Each enumerated action (upload, chat, calibrate, pair, OTA
// check, Wi-Fi scan, wake-ups, safety) is driven and asserted to finish in a
// visible, non-pending result - nothing just disappears.
import { test, expect } from '@playwright/test';
import { seedToken, openApp } from './_helpers.mjs';
import { STATE } from '../fixtures.mjs';

// A feedback element must settle into ok | none | error (never stuck pending).
async function settled(locator) {
  await expect(locator).toHaveAttribute('data-fb', /ok|none|error/);
}

test('upload ends with a visible result', async ({ page }) => {
  await seedToken(page); await openApp(page);
  await page.locator('.tab[data-p=memory]').click();
  await page.locator('#pane-mem details summary', { hasText: 'Files' }).click();
  await page.locator('#upFile').setInputFiles({ name: 'a.txt', mimeType: 'text/plain', buffer: Buffer.from('a') });
  await page.locator('#upBtn').click();
  await settled(page.locator('#upMsg'));
});

test('chat send ends with a visible reply, not a stuck spinner', async ({ page }) => {
  await seedToken(page);
  await page.route('**/api/chat', (route) => {
    if (route.request().method() === 'GET') return route.fulfill({ status: 200, contentType: 'application/json', body: '{"reply":"Hello from Nimbus."}' });
    return route.fulfill({ status: 200, contentType: 'application/json', body: '{"ok":true}' });
  });
  await openApp(page);
  await page.locator('.tab[data-p=chat]').click();
  await page.locator('#chatInput').fill('hi');
  await page.locator('#chatSend').click();
  await expect(page.locator('#chatLog')).toContainText('Hello from Nimbus.');
});

test('calibrate ends with a visible result', async ({ page }) => {
  await seedToken(page); await openApp(page);
  await page.locator('.tab[data-p=device]').click();
  await expect(page.locator('#battsec')).toBeVisible();
  await page.locator('#battsec > summary').click();
  page.once('dialog', (d) => d.accept());
  await page.locator('#battcalBtn').click();
  await settled(page.locator('#battcalMsg'));
});

test('cloud pair ends with a visible result', async ({ page }) => {
  await seedToken(page);
  await page.route('**/api/state', (route) => route.fulfill({ status: 200, contentType: 'application/json',
    body: JSON.stringify({ ...STATE, cloud: { line: 'Cloud access is off.', state: 'off', paired: false, optIn: false, code: '' } }) }));
  await openApp(page);
  await page.locator('.tab[data-p=device]').click();
  await page.locator('#pane-set details summary', { hasText: 'Cloud access' }).click();
  await page.locator('#cloudPair').click();
  await settled(page.locator('#cloudMsg'));
});

test('OTA check ends with a visible result', async ({ page }) => {
  await seedToken(page); await openApp(page);
  await page.locator('.tab[data-p=device]').click();
  await page.locator('#pane-set details summary', { hasText: 'Software update' }).click();
  await page.locator('#fwCheck').click();
  await settled(page.locator('#fwMsg'));
});

test('Wi-Fi scan ends with a visible result', async ({ page }) => {
  await seedToken(page);
  await page.route('**/api/wifi**', (route) => route.fulfill({ status: 200, contentType: 'application/json',
    body: JSON.stringify({ networks: [], scan: { scanning: false, networks: [{ ssid: 'Home', rssi: -50, enc: 1 }] } }) }));
  await openApp(page);
  await page.locator('.tab[data-p=device]').click();
  await page.locator('#pane-set details summary', { hasText: 'Connectivity' }).click();
  await page.locator('#scan').click();
  await expect(page.locator('#msg')).toContainText(/network/i);
});

test('wake-ups policy + guest moderation end with a visible result', async ({ page }) => {
  await seedToken(page); await openApp(page);
  await page.locator('.tab[data-p=assistant]').click();
  await page.locator('.subtab[data-sp=routines]').click();
  await page.locator('#wkPolicy').selectOption('ask');
  await settled(page.locator('#wkMsg'));
  await page.locator('.subtab[data-sp=safety]').click();
  await page.locator('#modInbound').check();
  await page.locator('#modSave').click();
  await expect(page.locator('#modmsg')).toHaveText(/saved/i);
});
