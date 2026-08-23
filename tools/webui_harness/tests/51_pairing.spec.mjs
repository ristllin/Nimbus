// CUM-51: pairing + cloud card - QR + large high-contrast code side by side,
// copy button, clear steps; the grayed-out code bug is gone.
import { test, expect } from '@playwright/test';
import { seedToken, openApp } from './_helpers.mjs';

async function openCloud(page) {
  await openApp(page);
  await page.locator('.tab[data-p=device]').click();
  await page.locator('#pane-set details summary', { hasText: 'Cloud access' }).click();
  await expect(page.locator('#cloudPairCard')).toBeVisible();
}

test('pairing shows the code large and high-contrast (not grayed out)', async ({ page }) => {
  await seedToken(page);
  await openCloud(page);
  const code = page.locator('#cloudCode');
  await expect(code).toHaveText('CN-4821-QK');
  // High-contrast + large: font-size well above body text, and the ink color, not
  // the dim hint color. This is the regression guard for the grayed-out bug.
  const { size, color } = await code.evaluate((el) => {
    const s = getComputedStyle(el);
    return { size: parseFloat(s.fontSize), color: s.color };
  });
  expect(size).toBeGreaterThan(22);
  // --ink is #eceef2 -> rgb(236, 238, 242); --ink3 (hint) is #6f7684. Assert it is
  // the bright ink, not the dim one.
  expect(color).toBe('rgb(236, 238, 242)');
});

test('a QR is rendered next to the code from /api/qr carrying the code', async ({ page }) => {
  await seedToken(page);
  let qrData = null;
  page.on('request', (r) => { const u = new URL(r.url()); if (u.pathname === '/api/qr') qrData = u.searchParams.get('data'); });
  await openCloud(page);
  await expect(page.locator('#cloudQr svg')).toBeVisible();
  await expect.poll(() => qrData).not.toBeNull();
  expect(qrData).toContain('CN-4821-QK');
  expect(qrData).toContain('app.cumulo-nimbus.ai');
});

test('the copy button copies the code', async ({ page, context }) => {
  await context.grantPermissions(['clipboard-read', 'clipboard-write']);
  await seedToken(page);
  await openCloud(page);
  await page.locator('#cloudCopy').click();
  const clip = await page.evaluate(() => navigator.clipboard.readText());
  expect(clip).toBe('CN-4821-QK');
});

test('clear steps are shown', async ({ page }) => {
  await seedToken(page);
  await openCloud(page);
  await expect(page.locator('#cloudPairCard ol li')).toHaveCount(3);
  await expect(page.locator('#cloudPairCard')).toContainText('app.cumulo-nimbus.ai');
});
