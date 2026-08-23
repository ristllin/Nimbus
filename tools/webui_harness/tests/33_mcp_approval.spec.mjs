// CUM-33 (cross-lane contract, rendered in N1's Assistant > Connectors): a
// device-dialed MCP server pending owner approval shows a pending state with
// Approve/Deny that flip the connectors-blob "appr" bit via the owner-gated
// /api/connectors write. Enforcement/persistence live in N4; N1 renders + writes.
import { test, expect } from '@playwright/test';
import { seedToken, openApp } from './_helpers.mjs';

async function openConnectors(page) {
  await openApp(page);
  await page.locator('.tab[data-p=assistant]').click();
  await expect(page.locator('#conncards')).toBeVisible();
}

test('a pending MCP server shows pending approval with Approve/Deny', async ({ page }) => {
  await seedToken(page);
  await openConnectors(page);
  const card = page.locator('#conncards', { hasText: 'devtools' });
  await expect(card).toContainText('pending approval');
  await expect(card.getByRole('button', { name: 'Approve' })).toBeVisible();
  await expect(card.getByRole('button', { name: 'Deny' })).toBeVisible();
});

test('Approve flips the appr bit via the owner-gated connectors write', async ({ page }) => {
  await seedToken(page);
  let patch = null;
  await page.route('**/api/connectors', async (route) => {
    const req = route.request();
    if (req.method() === 'POST') {
      patch = new URLSearchParams(req.postData() || '').get('patch');
      return route.fulfill({ status: 200, contentType: 'application/json', body: '{"ok":true}' });
    }
    return route.continue();
  });
  await openConnectors(page);
  await page.locator('#conncards').getByRole('button', { name: 'Approve' }).click();
  await expect.poll(() => patch).not.toBeNull();
  expect(JSON.parse(patch)).toMatchObject({ name: 'devtools', appr: 1 });
  await expect(page.locator('#cc_devtools_apprmsg')).toHaveAttribute('data-fb', 'ok');
});

test('Deny sends appr:0', async ({ page }) => {
  await seedToken(page);
  let patch = null;
  await page.route('**/api/connectors', async (route) => {
    const req = route.request();
    if (req.method() === 'POST') {
      patch = new URLSearchParams(req.postData() || '').get('patch');
      return route.fulfill({ status: 200, contentType: 'application/json', body: '{"ok":true}' });
    }
    return route.continue();
  });
  await openConnectors(page);
  await page.locator('#conncards').getByRole('button', { name: 'Deny' }).click();
  await expect.poll(() => patch).not.toBeNull();
  expect(JSON.parse(patch)).toMatchObject({ name: 'devtools', appr: 0 });
});
