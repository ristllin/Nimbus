// CUM-482 / CUM-507: the web chat's async results and turn states.
//   * a turn that only started sub-agents is DONE with no reply: its placeholder
//     goes away instead of waiting out the poll budget (or catching a later ack);
//   * an async result (a synthesis delivered BETWEEN turns, into the history)
//     appears without the owner sending another message - the page watches the
//     device's activity counter while background work is live;
//   * a host that reports no activity (Virtual Nimbus) is never watched, so the
//     watcher can never consume its legacy reply queue.
import { test, expect } from '@playwright/test';
import { seedToken, openApp } from './_helpers.mjs';

const row = (id, role, text, tags = '') =>
  ({ id, session: 'web', channel: 'web', role, kind: 'message', text, tags });

async function openChat(page) {
  await openApp(page);
  await page.locator('.tab[data-p=chat]').click();
  await expect(page.locator('#pane-chat')).toBeVisible();
}

test('a spawn-only turn drops its placeholder and the synthesis appears by itself', async ({ page }) => {
  await seedToken(page);
  let synthesized = false;
  let watches = 0;
  // History: the owner's message; once the synthesis has "landed", its row too.
  await page.route('**/api/mem/episodic**', (r) => {
    const ms = [row('m2', 'user', 'research timber fire rules')];
    if (synthesized) ms.unshift(row('m5', 'assistant', 'SYNTHESIS: the 7 m rule is MBO s.2(3).'));
    return r.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ messages: ms }) });
  });
  await page.route('**/api/chat**', async (route) => {
    const req = route.request();
    const json = (b) => route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(b) });
    if (req.method() === 'POST') return json({ ok: true, turn: 'w00000001' });
    const u = new URL(req.url());
    if (u.searchParams.get('turn')) return json({ turn: 'w00000001', reply: '', pending: false, done: true, gone: false, async: 3, busy: 2 });
    if (u.searchParams.get('watch')) {
      watches += 1;
      if (watches >= 2) synthesized = true;   // the synthesis lands between turns
      return json({ async: synthesized ? 4 : 3, busy: synthesized ? 0 : 2 });
    }
    return json({ reply: 'LEGACY-SLOT-MUST-NOT-BE-READ', pending: false });
  });
  await openChat(page);
  await page.locator('#chatInput').fill('research timber fire rules');
  await page.locator('#chatSend').click();
  // Done with no reply: the dim placeholder is removed, nothing stale fills it.
  await expect(page.locator('#chatMsg')).toHaveText('', { timeout: 8000 });
  await expect(page.locator('#chatLog')).not.toContainText('LEGACY-SLOT');
  await expect(page.locator('#chatLog')).not.toContainText('…');
  // No further message from the owner - the watcher brings the synthesis in.
  await expect(page.locator('#chatLog')).toContainText('SYNTHESIS: the 7 m rule', { timeout: 20000 });
});

test('a host that reports no activity is never watched (its reply queue stays unread)', async ({ page }) => {
  await seedToken(page);
  let watched = 0;
  await page.route('**/api/mem/episodic**', (r) =>
    r.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ messages: [] }) }));
  await page.route('**/api/chat**', async (route) => {
    const req = route.request();
    const json = (b) => route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(b) });
    if (req.method() === 'POST') return json({ pending: true, turn: 1 });
    const u = new URL(req.url());
    if (u.searchParams.get('watch')) watched += 1;
    if (u.searchParams.get('turn')) return json({ reply: 'hosted answer', pending: false });
    return json({ reply: '', pending: false });   // no `busy`: not this firmware
  });
  await openChat(page);
  await page.locator('#chatInput').fill('hello');
  await page.locator('#chatSend').click();
  await expect(page.locator('#chatLog')).toContainText('hosted answer');
  await page.waitForTimeout(9000);   // two watcher intervals
  expect(watched).toBeLessThanOrEqual(1);   // at most the page-load probe, never a loop
});
