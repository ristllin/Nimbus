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

// CUM-514 (L20 docs audit): the device's REAL /api/wakeups contract
// (src/net/webui.cpp + lib/core dumpWakeupsApi): GET {approvalMode, wakeups[]},
// POST action=mode&value=ask|auto and action=approve|cancel&id. These specs used
// to stub the page's own (wrong) shape, so they passed while the control was dead.
const wk = (over = {}) => ({ approvalMode: 'auto', maxArmed: 4, armed: 0, wakeups: [], ...over });

test('wake-ups default to silent-allow and the policy is saveable', async ({ page }) => {
  await seedToken(page);
  let post = null;
  await page.route('**/api/wakeups', (route) => {
    const req = route.request();
    if (req.method() === 'POST') { post = new URLSearchParams(req.postData() || ''); return route.fulfill({ status: 200, body: '{"ok":true}' }); }
    return route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(wk()) });
  });
  await openSub(page, 'routines');
  await expect(page.locator('#wkPolicy')).toBeVisible();
  await expect(page.locator('#wkPolicy')).toHaveValue('auto');
  await page.locator('#wkPolicy').selectOption('ask');
  await expect.poll(() => post && post.get('action')).toBe('mode');
  expect(post.get('value')).toBe('ask');
  await expect(page.locator('#wkMsg')).toContainText('Ask me first');
});

test('"ask me first" shows a single approval card, not a loop', async ({ page }) => {
  await seedToken(page);
  const posts = [];
  await page.route('**/api/wakeups', (route) => {
    const req = route.request();
    if (req.method() === 'POST') { posts.push(new URLSearchParams(req.postData() || '')); return route.fulfill({ status: 200, body: '{"ok":true}' }); }
    return route.fulfill({ status: 200, contentType: 'application/json',
      body: JSON.stringify(wk({ approvalMode: 'ask', armed: 2, wakeups: [
        { id: 'lp0009', name: 'Follow up on the build', note: 'Check whether CI passed.', createdBy: 'agent', enabled: true, approved: false, pending: true, nextRun: 1, inSec: 600, lastResult: 'none' },
        { id: 'lp0010', name: 'Second pending', note: 'x', createdBy: 'agent', enabled: true, approved: false, pending: true, nextRun: 1, inSec: 900, lastResult: 'none' },
      ] })) });
  });
  await openSub(page, 'routines');
  await expect(page.locator('#wkPolicy')).toHaveValue('ask');
  const card = page.locator('#wkPending');
  await expect(card).toBeVisible();
  await expect(card).toContainText('Follow up on the build');
  await expect(card).toContainText('in ~10 min');
  await expect(card).toContainText('Check whether CI passed.');
  // Exactly one approval card (a single yes/no), never a repeating list of prompts.
  await expect(page.locator('#wkPending')).toHaveCount(1);
  await expect(card.getByRole('button', { name: 'Approve' })).toBeVisible();
  await card.getByRole('button', { name: 'Approve' }).click();
  await expect.poll(() => posts.length).toBe(1);
  expect(posts[0].get('action')).toBe('approve');
  expect(posts[0].get('id')).toBe('lp0009');
  await card.getByRole('button', { name: 'Deny' }).click();
  await expect.poll(() => posts.length).toBe(2);
  expect(posts[1].get('action')).toBe('cancel');   // the API's deny verb
  expect(posts[1].get('id')).toBe('lp0009');
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
