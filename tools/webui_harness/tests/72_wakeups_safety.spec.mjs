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

// GATE2 fix pass (review finding over F6): the API answers 200 {"ok":false}
// when the id no longer exists - the wake-up fired and was auto-removed
// between the page's GET and the owner's click (the stale-row case). The card
// used to render "Approved."/"Denied." in the green ok state for that no-op.
// Both verbs must surface the no-op instead; the class, not one button.
test('a stale approval (ok:false) is surfaced, never "Approved." or "Denied."', async ({ page }) => {
  await seedToken(page);
  const posts = [];
  await page.route('**/api/wakeups', (route) => {
    const req = route.request();
    if (req.method() === 'POST') { posts.push(new URLSearchParams(req.postData() || '')); return route.fulfill({ status: 200, body: '{"ok":false}' }); }
    return route.fulfill({ status: 200, contentType: 'application/json',
      body: JSON.stringify(wk({ approvalMode: 'ask', wakeups: [
        { id: 'lp0042', name: 'Follow up on the build', note: 'Check whether CI passed.', createdBy: 'agent', enabled: true, approved: false, pending: true, nextRun: 1, inSec: 600, lastResult: 'none' },
      ] })) });
  });
  await openSub(page, 'routines');
  const card = page.locator('#wkPending');
  await expect(card).toBeVisible();
  const msg = page.locator('#wkPendMsg');
  await card.getByRole('button', { name: 'Approve' }).click();
  await expect.poll(() => posts.length).toBe(1);
  expect(posts[0].get('action')).toBe('approve');
  expect(posts[0].get('id')).toBe('lp0042');
  await expect(msg).toContainText('no longer pending');
  await expect(msg).not.toContainText('Approved.');
  await expect(msg).not.toHaveAttribute('data-fb', 'ok');
  // The deny verb lies the same way: the whole card, not one button.
  await card.getByRole('button', { name: 'Deny' }).click();
  await expect.poll(() => posts.length).toBe(2);
  expect(posts[1].get('action')).toBe('cancel');
  expect(posts[1].get('id')).toBe('lp0042');
  await expect(msg).toContainText('no longer pending');
  await expect(msg).not.toContainText('Denied.');
  await expect(msg).not.toHaveAttribute('data-fb', 'ok');
});

// GATE2 fix pass (self-review finding): the same honest {"ok":false} answer
// lands on /api/loops row actions (nimbusd, a stale row: pause/resume/delete/
// approve of an id that no longer exists), and the row buttons used to swallow
// it entirely - no ok check, no catch, just a silent reload. A no-op (or a
// failed request) must say so next to the list it reloaded.
test('a stale routine row action (ok:false) is surfaced, not silent', async ({ page }) => {
  await seedToken(page);
  const posts = [];
  await page.route('**/api/loops', (route) => {
    const req = route.request();
    if (req.method() === 'POST') { posts.push(req.postData() || ''); return route.fulfill({ status: 200, body: '{"ok":false}' }); }
    return route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify([
      { id: 'lp0007', name: 'Daily digest', prompt: 'Summarize the day', kind: 'interval', everySec: 86400, at: '', enabled: true, why: '', nextRun: 0, byAgent: false, approved: true, lastResult: 'ok', firesToday: 1, tokensToday: 120, reserved: false },
    ]) });
  });
  await openSub(page, 'routines');
  const pause = page.locator('#loopList button[data-act=pause]');
  await expect(pause).toBeVisible();
  await pause.click();
  await expect.poll(() => posts.length).toBe(1);
  expect(posts[0]).toContain('pause');
  expect(posts[0]).toContain('lp0007');
  await expect(page.locator('#lpMsg')).toContainText('no longer available');
  await expect(page.locator('#lpMsg')).not.toHaveAttribute('data-fb', 'ok');
});
