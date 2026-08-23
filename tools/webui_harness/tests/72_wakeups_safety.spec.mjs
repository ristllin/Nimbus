// CUM-72: wake-ups + safety settings. Wake-ups: default silent-allow; "ask me
// first" surfaces a SINGLE approval card (never a loop). Safety: three moderation
// gates + a cost note.
import { test, expect } from '@playwright/test';
import { seedToken, openApp } from './_helpers.mjs';

async function openAssistant(page) {
  await openApp(page);
  await page.locator('.tab[data-p=assistant]').click();
  await expect(page.locator('#wkPolicy')).toBeVisible();
}

test('wake-ups default to silent-allow and the policy is saveable', async ({ page }) => {
  await seedToken(page);
  let policy = null;
  await page.route('**/api/wakeups', (route) => {
    const req = route.request();
    if (req.method() === 'POST') { policy = new URLSearchParams(req.postData() || '').get('policy'); return route.fulfill({ status: 200, body: '{"ok":true}' }); }
    return route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ policy: 'silent-allow', items: [], pending: null }) });
  });
  await openAssistant(page);
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
  await openAssistant(page);
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

test('safety shows three moderation gates + a cost note, each saveable', async ({ page }) => {
  await seedToken(page);
  const posts = [];
  await page.route('**/api/safety', (route) => {
    const req = route.request();
    if (req.method() === 'POST') { const p = new URLSearchParams(req.postData() || ''); posts.push([p.get('gate'), p.get('on')]); return route.fulfill({ status: 200, body: '{"ok":true}' }); }
    return route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ input: false, output: false, media: false, costNote: 'Each gate adds a small provider call.' }) });
  });
  await openAssistant(page);
  await expect(page.locator('#safeIn')).toBeVisible();
  await expect(page.locator('#safeOut')).toBeVisible();
  await expect(page.locator('#safeMedia')).toBeVisible();
  await expect(page.locator('#safeCost')).toContainText('provider call');
  await page.locator('#safeOut').check();
  await expect.poll(() => posts).toContainEqual(['output', '1']);
});
