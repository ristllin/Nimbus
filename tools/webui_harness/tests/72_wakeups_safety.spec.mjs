// CUM-72 / CUM-163: wake-ups + safety, now on the Assistant page's exclusive
// subtabs. Wake-ups live on the Routines subtab: default silent-allow; "ask me
// first" surfaces a SINGLE approval card (never a loop). Safety is its own subtab
// and shows only the REAL controls - the Downloads trust policy and the guest
// moderation gates (wired to /api/orch). The old safeIn/safeOut/safeMedia gates
// were retired (they posted to /api/safety, which no firmware ever served).
import { test, expect } from '@playwright/test';
import { seedToken, openApp } from './_helpers.mjs';
import { ORCH } from '../fixtures.mjs';

async function openSub(page, sp) {
  await openApp(page);
  await page.locator('.tab[data-p=assistant]').click();
  await page.locator(`.subtab[data-sp=${sp}]`).click();
}

test('wake-ups default to silent-allow and the policy is saveable', async ({ page }) => {
  await seedToken(page);
  let policy = null;
  await page.route('**/api/wakeups', (route) => {
    const req = route.request();
    if (req.method() === 'POST') { policy = new URLSearchParams(req.postData() || '').get('policy'); return route.fulfill({ status: 200, body: '{"ok":true}' }); }
    return route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ policy: 'silent-allow', items: [], pending: null }) });
  });
  await openSub(page, 'routines');
  await expect(page.locator('#wkPolicy')).toBeVisible();
  await expect(page.locator('#wkPolicy')).toHaveValue('silent-allow');
  await page.locator('#wkPolicy').selectOption('ask');
  await expect.poll(() => policy).toBe('ask');
});

test('"ask me first" shows a single approval card, not a loop', async ({ page }) => {
  await seedToken(page);
  let action = null;
  await page.route('**/api/wakeups', (route) => {
    const req = route.request();
    if (req.method() === 'POST') { action = new URLSearchParams(req.postData() || '').get('action'); return route.fulfill({ status: 200, body: '{"ok":true}' }); }
    return route.fulfill({ status: 200, contentType: 'application/json',
      body: JSON.stringify({ policy: 'ask', items: [], pending: { id: 'w9', label: 'Follow up on the build', when: '18:00', why: 'Check whether CI passed.' } }) });
  });
  await openSub(page, 'routines');
  const card = page.locator('#wkPending');
  await expect(card).toBeVisible();
  await expect(card).toContainText('Follow up on the build');
  // Exactly one approval card (a single yes/no), never a repeating list of prompts.
  await expect(page.locator('#wkPending')).toHaveCount(1);
  await expect(card.getByRole('button', { name: 'Approve' })).toBeVisible();
  await expect(card.getByRole('button', { name: 'Deny' })).toBeVisible();
  await card.getByRole('button', { name: 'Approve' }).click();
  await expect.poll(() => action).toBe('approve');
});

test('safety subtab shows the real controls (downloads + guest moderation), no dead gates', async ({ page }) => {
  await seedToken(page);
  const posts = [];
  await page.route('**/api/orch', (route) => {
    const req = route.request();
    if (req.method() === 'POST') { posts.push(new URLSearchParams(req.postData() || '')); return route.fulfill({ status: 200, body: '{"ok":true}' }); }
    return route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(ORCH) });
  });
  await openSub(page, 'safety');
  // Real controls present.
  await expect(page.locator('#fetchpol')).toBeVisible();
  await expect(page.locator('#modInbound')).toBeVisible();
  await expect(page.locator('#modOutbound')).toBeVisible();
  await expect(page.locator('#modInjection')).toBeVisible();
  // The retired dead gates are gone.
  await expect(page.locator('#safeIn')).toHaveCount(0);
  await expect(page.locator('#safeOut')).toHaveCount(0);
  await expect(page.locator('#safeMedia')).toHaveCount(0);
  // A guest-moderation gate saves via /api/orch.
  await page.locator('#modInbound').check();
  await page.locator('#modSave').click();
  await expect.poll(() => posts.some((p) => p.get('modInbound') === '1')).toBe(true);
});
