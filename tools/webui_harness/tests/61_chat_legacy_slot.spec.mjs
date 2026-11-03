// Bench F03 (v4.9.0, lane POL1): the legacy GET /api/chat single slot. The
// reply is delivered while the turn is still wrapping up (busy=1); a client
// whose loop keeps polling until pending=false AND busy=0 (the bench helper,
// HIL legs) had its interim poll eat the reply, and its final poll read an
// empty slot - the reply was lost with no way back.
//
// The fixed device contract (agent::WebLegacySlot, host-pinned in
// test_harness_webchat): a poll while work still runs PEEKS (reads without
// retiring), an idle poll consumes. These specs run the bench's exact client
// loop in a real browser against a mock implementing that contract, and pin
// the web app's own legacy fallback path (a POST answer with no turn id).
//
// CHAT_CONTRACT=old runs the same specs against the pre-fix consume-on-read
// contract. It is NOT the default and not CI: it exists to capture the red
// side of the fix (the bench loop's final poll reads an empty reply) - run it
// once, save the failure log as evidence, then run the default (fixed) green.
import { test, expect } from '@playwright/test';
import { seedToken, openApp } from './_helpers.mjs';

const REPLY = 'At most 155 g of protein is left today.';
const OLD_CONTRACT = process.env.CHAT_CONTRACT === 'old';

// The device's legacy-slot timeline, in ms from the POST:
//   0..1000    turn running, reply not delivered yet  -> pending, busy
//   1000..2500 reply delivered, turn still wrapping  -> peek window (busy)
//   2500..     turn over                             -> idle; first poll consumes
const DELIVERED_AT = 1000;
const IDLE_AT = 2500;

// Mock GET /api/chat (no params) with the device contract. `state.retired` is
// the slot's consumed flag (one per TURN, shared by every poll); under the old
// contract ANY read retires it.
function legacyGet(state, elapsed) {
  const delivered = elapsed >= DELIVERED_AT;
  const busy = elapsed < IDLE_AT;
  const has = delivered && !state.retired;
  if (has && (OLD_CONTRACT || !busy)) state.retired = true;
  return { reply: has ? REPLY : '', pending: !delivered, busy: busy ? 1 : 0 };
}

// The bench client loop (gf_helpers.turn, evidence/bench-5.0.0/rc-f7553b56/F03):
// poll, keep only the LAST body, stop when pending=false AND busy=0.
async function benchClientLoop(page) {
  return page.evaluate(async () => {
    let last = null;
    for (let i = 0; i < 40; i++) {
      const r = await fetch('/api/chat');
      last = await r.json();
      if (last.pending === false && last.busy === 0) break;
      await new Promise((res) => setTimeout(res, 300));
    }
    return last;
  });
}

test('the bench client loop reads the reply on its final poll (F03)', async ({ page }) => {
  await seedToken(page);
  await openApp(page);
  let t0 = 0;
  const slot = { retired: false };
  await page.route('**/api/chat', async (route) => {
    const req = route.request();
    const json = (b) => route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(b) });
    if (req.method() === 'POST') { t0 = Date.now(); return json({ ok: true, turn: 'w00000001' }); }
    if (new URL(req.url()).searchParams.get('turn'))
      return json({ turn: 'w00000001', reply: REPLY, pending: false, done: true, gone: false });
    return json(legacyGet(slot, Date.now() - t0));
  });
  await page.evaluate(() => fetch('/api/chat', { method: 'POST', body: new URLSearchParams({ text: 'how much protein is left today?' }) }));
  const last = await benchClientLoop(page);
  // The client's final poll (pending false, busy 0) must still carry the reply:
  // under the old contract its own busy-window poll had already eaten it.
  expect(last.pending).toBe(false);
  expect(last.busy).toBe(0);
  expect(last.reply).toBe(REPLY);
});

test('the web app renders the reply over the legacy fallback (busy-window peek)', async ({ page }, testInfo) => {
  await seedToken(page);
  let t0 = 0;
  const slot = { retired: false };
  await page.route('**/api/chat', async (route) => {
    const req = route.request();
    const json = (b) => route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(b) });
    // No turn id in the POST answer: the pre-CUM-482 firmware shape, so the
    // page polls the legacy slot (the exact endpoint F03 lost replies on).
    if (req.method() === 'POST') { t0 = Date.now(); return json({ pending: true }); }
    return json(legacyGet(slot, Date.now() - t0));
  });
  await openApp(page);
  await page.locator('.tab[data-p=chat]').click();
  await expect(page.locator('#pane-chat')).toBeVisible();
  await page.locator('#chatInput').fill('how much protein is left today?');
  await page.locator('#chatSend').click();
  // The reply lands in the owner's chat log even though it was delivered
  // inside the busy window (the peek never retired it).
  await expect(page.locator('#chatLog')).toContainText(REPLY, { timeout: 15000 });
  await page.screenshot({ path: `screenshots/61-f03-legacy-reply-${testInfo.project.name}.png`, fullPage: true });
});
