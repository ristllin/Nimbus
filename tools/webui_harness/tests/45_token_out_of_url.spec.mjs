// CUM-45: the access token is out of URLs.
//  - downloads ride the X-Nimbus-Token header (no ?t=/&t= links)
//  - a legacy ?t= sign-in link is accepted once, stripped, and warns
//  - a one-time ?c= code is exchanged for the token over POST
//  - naming: "Device sign-in code" (code 1), "Cloud link code" (code 2), "Sign-in QR"
import { test, expect } from '@playwright/test';
import { seedToken, collectErrors, openApp } from './_helpers.mjs';

test('file downloads carry the token in a header, never in a URL', async ({ page }) => {
  await seedToken(page);
  const { errors } = collectErrors(page);
  await openApp(page);
  // Open Memory & Files (the mem pane lazy-loads the file list), then expand the
  // collapsed Files section so its table is on screen.
  await page.locator('nav.tabs .tab[data-p=memory]').click();
  await page.locator('#pane-mem details summary', { hasText: 'Files' }).click();
  await expect(page.locator('#filesList table')).toBeVisible();
  // No anchor anywhere may carry a token query param.
  const hrefs = await page.locator('a[href]').evaluateAll((as) => as.map((a) => a.getAttribute('href')));
  for (const h of hrefs) expect(h, `token leaked in href: ${h}`).not.toMatch(/[?&]t=/);

  // Clicking "get" must issue a fetch to /api/files/dl with the header set and
  // no ?t= in the request URL.
  let dlReq = null;
  page.on('request', (r) => { if (r.url().includes('/api/files/dl')) dlReq = r; });
  await page.locator('#filesList a[data-dlp]').first().click();
  await expect.poll(() => dlReq).not.toBeNull();
  expect(dlReq.url()).not.toMatch(/[?&]t=/);
  expect(dlReq.headers()['x-nimbus-token']).toBeTruthy();
  expect(errors, errors.join('\n')).toEqual([]);
});

test('a legacy ?t= link signs in once, strips the URL, and shows a migration hint', async ({ page }) => {
  // Do NOT seed a token; the ?t= link is the sign-in.
  await page.goto('/?t=LEGACYTOKEN99');
  // The token is stripped from the address bar synchronously.
  await expect.poll(() => new URL(page.url()).search).toBe('');
  // Migration hint is shown.
  await expect(page.locator('#migHint')).toBeVisible();
  await expect(page.locator('#migHint')).toContainText('Sign-in QR');
  // The stored token now rides the header on API requests.
  let stateReq = null;
  page.on('request', (r) => { if (r.url().endsWith('/api/state')) stateReq = r; });
  await page.waitForTimeout(500);
  await expect.poll(() => stateReq).not.toBeNull();
  expect(stateReq.headers()['x-nimbus-token']).toBe('LEGACYTOKEN99');
  // Dismissible.
  await page.locator('#migHint button').click();
  await expect(page.locator('#migHint')).toHaveCount(0);
});

test('a one-time ?c= code is exchanged for the token over POST', async ({ page }) => {
  let exchange = null;
  await page.route('**/api/signin/exchange', async (route) => {
    exchange = route.request();
    await route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ token: 'EXCHANGED123' }) });
  });
  await page.goto('/?c=ONETIMECODE');
  await expect.poll(() => exchange).not.toBeNull();
  expect(exchange.method()).toBe('POST');
  // The code is stripped from the URL.
  await expect.poll(() => new URL(page.url()).search).toBe('');
});

test('incognito ?c= sign-in resumes without getting stuck behind the gate', async ({ page }) => {
  // Simulate blocked storage (private browsing): setItem throws, so the token is
  // memory-only. The exchange must still dismiss the gate and resume.
  await page.addInitScript(() => { Storage.prototype.setItem = function () { throw new Error('blocked'); }; });
  await page.route('**/api/signin/exchange', (r) => r.fulfill({ status: 200, contentType: 'application/json', body: '{"token":"MEMTOK1"}' }));
  let stateTok = null;
  page.on('request', (r) => { if (r.url().endsWith('/api/state')) stateTok = r.headers()['x-nimbus-token']; });
  await page.goto('/?c=ONECODE');
  await expect(page.locator('#authgate')).toHaveCount(0);
  await expect.poll(() => stateTok).toBe('MEMTOK1');
});

test('naming: device sign-in code, cloud link code, sign-in QR', async ({ page }) => {
  await seedToken(page);
  await openApp(page);
  const html = await page.content();
  expect(html).toContain('Device sign-in code');
  expect(html).toContain('Sign-in QR');
  // The legacy terms are gone from the web UI copy.
  expect(html).not.toContain('Recovery access token');
  expect(html).not.toContain('Generate New Token');
});
