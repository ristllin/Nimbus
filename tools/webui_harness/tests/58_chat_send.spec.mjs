// CUM-218: chat send robustness under mid-turn traffic. The owner rapid-tapped on
// his phone and the app dropped a message silently and rendered a stale reply ("6")
// under a newer message. These specs pin the honest contract of the send path:
//   * a sent message shows an honest pending state immediately (its own user bubble
//     + a "thinking" indicator), even while a prior turn is still in flight;
//   * a message sent mid-turn is NEVER silently lost;
//   * each reply is matched to ITS OWN turn by the id POST hands back - never the
//     "latest reply", so a prior turn's answer can never surface under a newer one;
//   * the Send button stays honest (usable, never a dead no-op) during a turn.
import { test, expect } from '@playwright/test';
import { seedToken, openApp } from './_helpers.mjs';

async function openChat(page) {
  await openApp(page);
  await page.locator('.tab[data-p=chat]').click();
  await expect(page.locator('#pane-chat')).toBeVisible();
}

// Wire a turn-aware /api/chat mock: POST assigns a monotonic turn id and returns it;
// GET ?turn=<id> answers ONLY that turn (pending until "ready", then its own reply).
// A GET with NO turn id returns a deliberately WRONG reply, so a client that fails to
// match by turn (the old "latest reply" bug) is caught red-handed. Episodic reload is
// aborted so the canonical-store re-sync can't wipe the bubbles mid-assertion.
async function wireChat(page) {
  await page.route('**/api/mem/episodic**', (r) => r.abort());
  let turnN = 0;
  const replyFor = {};
  await page.route('**/api/chat**', async (route) => {
    const req = route.request();
    if (req.method() === 'POST') {
      turnN += 1;
      replyFor[turnN] = turnN === 1 ? 'answer-one' : 'answer-two';
      return route.fulfill({ status: 200, contentType: 'application/json',
        body: JSON.stringify({ pending: true, turn: turnN }) });
    }
    const t = new URL(req.url()).searchParams.get('turn');
    if (!t) {
      return route.fulfill({ status: 200, contentType: 'application/json',
        body: JSON.stringify({ reply: 'STALE-WRONG', pending: false }) });
    }
    const body = replyFor[t] ? { reply: replyFor[t], pending: false } : { reply: '', pending: true };
    return route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(body) });
  });
}

test('sending shows an honest pending state, then the reply fills its own bubble', async ({ page }) => {
  await seedToken(page);
  await wireChat(page);
  await openChat(page);
  await page.locator('#chatInput').fill('first message');
  await page.locator('#chatSend').click();
  // Honest pending: the user bubble is there at once, the indicator says so, and the
  // Send button is not a dead no-op.
  await expect(page.locator('#chatLog')).toContainText('first message');
  await expect(page.locator('#chatMsg')).toContainText('Thinking');
  await expect(page.locator('#chatSend')).toBeEnabled();
  // The reply lands in its own bubble - matched by turn id, never the stale "latest".
  await expect(page.locator('#chatLog')).toContainText('answer-one');
  await expect(page.locator('#chatLog')).not.toContainText('STALE-WRONG');
  await expect(page.locator('#chatMsg')).toHaveText('');   // indicator clears when idle
});

test('a message sent mid-turn is not lost and each reply matches its own turn', async ({ page }) => {
  await seedToken(page);
  await wireChat(page);
  await openChat(page);
  await page.locator('#chatInput').fill('first message');
  await page.locator('#chatSend').click();
  await expect(page.locator('#chatLog')).toContainText('first message');
  // Send a SECOND message while the first turn is still in flight. It must appear at
  // once (its own bubble), never be silently swallowed.
  await page.locator('#chatInput').fill('second message');
  await page.locator('#chatSend').click();
  await expect(page.locator('#chatLog')).toContainText('second message');
  // Both replies arrive, each under ITS OWN message. The DOM order proves the match:
  // first -> answer-one -> second -> answer-two. A stale replay would drop answer-two
  // into the first turn's bubble (before "second message") and fail this ordering.
  await expect(page.locator('#chatLog')).toContainText('answer-one');
  await expect(page.locator('#chatLog')).toContainText('answer-two');
  await expect(page.locator('#chatLog')).not.toContainText('STALE-WRONG');
  const all = await page.locator('#chatLog').innerText();
  expect(all.indexOf('first message')).toBeLessThan(all.indexOf('answer-one'));
  expect(all.indexOf('answer-one')).toBeLessThan(all.indexOf('second message'));
  expect(all.indexOf('second message')).toBeLessThan(all.indexOf('answer-two'));
});
