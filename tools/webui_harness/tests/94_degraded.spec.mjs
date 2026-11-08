// Lane DEGRADE: the web page says when the device runs degraded, the same set its screen
// shows (/api/state "degraded"). One short line per condition on Home, the reason one
// click away, cleared when the device recovers, and calm: no toast, no motion.
import { test, expect } from '@playwright/test';
import { seedToken, openApp, assertPane } from './_helpers.mjs';
import { STATE } from '../fixtures.mjs';

const SHOTS = process.env.DEGRADE_SHOTS || 'screenshots';

// The device's own words (nimbus/orch/degraded.h), as /api/state serves them.
const FALLBACK = {
  id: 'head_fallback', line: 'Anthropic down, Mistral answers',
  why: 'Anthropic, your main provider, is out of credit. Mistral, your 2nd choice, answered instead. Add credit with the provider. Turns return to Anthropic once it answers again.',
};
const PARKED = { id: 'provider_parked', line: 'OpenAI is out of credit',
  why: 'OpenAI is out of credit. Add credit with the provider. Other providers answer meanwhile, and it is checked again by itself.' };
const MEMORY = { id: 'memory', line: 'Memory is keyword-only, saves queued',
  why: 'Embeddings are unavailable, so memory search matches words only and 3 new memories wait in a queue. Nothing is lost: queued memories are stored once embeddings answer again. Check the provider\'s credit and key under Assistant > Models.' };
const CLOCK = { id: 'clock_unsynced', line: 'Clock not synced',
  why: 'The device has not reached a time server yet, so routines, the nightly dream and dated memories wait. It keeps trying.' };

const degraded = (...items) => ({ count: items.length, gen: items.length, items });

// A mutable device: the page's own poll (and loadState) reads whatever `dev.degraded` is.
async function routeState(page, dev) {
  await page.route('**/api/state', (r) => r.fulfill({ json: { ...STATE, degraded: dev.degraded } }));
}

const bar = (page) => page.locator('#degradedBar');

async function shot(page, info, name) {
  await page.screenshot({ path: `${SHOTS}/${info.project.name}-degraded-${name}.png` });
}

test('a healthy device shows no degraded banner', async ({ page }, info) => {
  await seedToken(page);
  await routeState(page, { degraded: degraded() });
  await openApp(page);
  await assertPane(page, 'home');
  await expect(page.locator('#devtiles')).toBeVisible();
  await expect(bar(page)).toHaveCount(0);
  await shot(page, info, 'healthy');
});

test('a state with no degraded key (an older device) shows nothing and throws nothing', async ({ page }) => {
  await seedToken(page);
  const errors = [];
  page.on('pageerror', (e) => errors.push(e.message));
  await page.route('**/api/state', (r) => {
    const { degraded, ...rest } = STATE;
    return r.fulfill({ json: rest });
  });
  await openApp(page);
  await expect(page.locator('#devtiles')).toBeVisible();
  await expect(bar(page)).toHaveCount(0);
  expect(errors).toEqual([]);
});

test('one line per condition, the device words verbatim, the reason one click away', async ({ page }, info) => {
  await seedToken(page);
  const dev = { degraded: degraded(FALLBACK, PARKED, CLOCK) };
  await routeState(page, dev);
  await openApp(page);
  await assertPane(page, 'home');
  await expect(bar(page)).toBeVisible();
  const rows = bar(page).locator('details');
  await expect(rows).toHaveCount(3);
  await expect(rows.nth(0).locator('summary')).toHaveText(FALLBACK.line);
  await expect(rows.nth(1).locator('summary')).toHaveText(PARKED.line);
  await expect(rows.nth(2).locator('summary')).toHaveText(CLOCK.line);
  // Reasons start closed, and open on a click, one at a time.
  await expect(rows.nth(0).locator('p')).toBeHidden();
  await shot(page, info, 'three-closed');
  await rows.nth(0).locator('summary').click();
  await expect(rows.nth(0).locator('p')).toBeVisible();
  await expect(rows.nth(0).locator('p')).toHaveText(FALLBACK.why);
  await expect(rows.nth(1).locator('p')).toBeHidden();
  await shot(page, info, 'three-first-open');
});

test('an open reason stays open across the poll, and the banner clears on recovery', async ({ page }, info) => {
  await seedToken(page);
  const dev = { degraded: degraded(FALLBACK) };
  await routeState(page, dev);
  await openApp(page);
  const row = bar(page).locator('details').first();
  await row.locator('summary').click();
  await expect(row.locator('p')).toBeVisible();
  // The device's reason moves with no change of line (a silent update), and a second
  // condition arrives: the row the owner is reading is the same element and stays open.
  dev.degraded = degraded({ ...FALLBACK, why: 'A reason that changed in place.' }, MEMORY);
  await page.evaluate(() => loadState());
  await expect(bar(page).locator('details')).toHaveCount(2);
  await expect(row.locator('p')).toHaveText('A reason that changed in place.');
  await expect(row.locator('p')).toBeVisible();
  await expect(bar(page).locator('details').nth(1).locator('summary')).toHaveText(MEMORY.line);
  await shot(page, info, 'two-one-open');
  // The first recovers: its row goes, the other stays.
  dev.degraded = degraded(MEMORY);
  await page.evaluate(() => loadState());
  await expect(bar(page).locator('details')).toHaveCount(1);
  await expect(bar(page).locator('details summary')).toHaveText(MEMORY.line);
  // Everything recovers: the banner is gone.
  dev.degraded = degraded();
  await page.evaluate(() => loadState());
  await expect(bar(page)).toBeHidden();
});

test('the banner follows the device order when a condition arrives in the middle', async ({ page }) => {
  await seedToken(page);
  const dev = { degraded: degraded(PARKED, CLOCK) };
  await routeState(page, dev);
  await openApp(page);
  await expect(bar(page).locator('details')).toHaveCount(2);
  dev.degraded = degraded(FALLBACK, PARKED, CLOCK);
  await page.evaluate(() => loadState());
  await expect(bar(page).locator('details')).toHaveCount(3);
  await expect(bar(page).locator('details summary')).toHaveText([FALLBACK.line, PARKED.line, CLOCK.line]);
});

test('the device text is shown as text, never as markup', async ({ page }) => {
  await seedToken(page);
  const hostile = { id: 'memory', line: '<img src=x onerror="window.__pwn=1">Memory', why: '<script>window.__pwn=1</script>why' };
  await routeState(page, { degraded: degraded(hostile) });
  await openApp(page);
  await expect(bar(page).locator('summary')).toHaveText(hostile.line);
  expect(await page.evaluate(() => window.__pwn)).toBeUndefined();
  await expect(bar(page).locator('img, script')).toHaveCount(0);
});

// The page's toast is <div id=toast> (toast() in ui_js.h sets its text and adds the class
// "show"); nothing ever has the class "toast". A MutationObserver on the real element sees
// every way it could be touched, whichever code path asks.
async function watchToast(page) {
  await page.evaluate(() => {
    window.__toastMutations = 0;
    new MutationObserver((rs) => { window.__toastMutations += rs.length; })
      .observe(document.getElementById('toast'), { attributes: true, childList: true, characterData: true, subtree: true });
  });
}
const toastMutations = (page) => page.evaluate(() => new Promise((res) => setTimeout(() => res(window.__toastMutations), 50)));

test('calm: the banner never toasts, alerts or opens a dialog, and a recovery says nothing', async ({ page }) => {
  await seedToken(page);
  const dev = { degraded: degraded() };
  await routeState(page, dev);
  await openApp(page);
  const dialogs = [];
  page.on('dialog', (d) => { dialogs.push(d.message()); d.dismiss(); });
  await watchToast(page);
  const focusBefore = await page.evaluate(() => document.activeElement && document.activeElement.tagName);
  dev.degraded = degraded(FALLBACK, MEMORY);
  await page.evaluate(() => loadState());
  await expect(bar(page)).toBeVisible();
  dev.degraded = degraded();
  await page.evaluate(() => loadState());
  await expect(bar(page)).toBeHidden();
  expect(await toastMutations(page)).toBe(0);
  await expect(page.locator('#toast')).not.toHaveClass(/show/);
  await expect(page.locator('#toast')).toHaveText('');
  expect(dialogs).toEqual([]);
  await expect(page.locator('#modalOv')).not.toHaveClass(/show|open/);
  // It never takes the keyboard, and it is a polite status region, never an alert.
  expect(await page.evaluate(() => document.activeElement && document.activeElement.tagName)).toBe(focusBefore);
  dev.degraded = degraded(FALLBACK);
  await page.evaluate(() => loadState());
  await expect(bar(page)).toBeVisible();
  await expect(bar(page)).toHaveAttribute('role', 'status');
  await expect(page.locator('#degradedBar [role=alert], #degradedBar [aria-live=assertive]')).toHaveCount(0);
  // The detector is live: the page's own toast() is seen by it (so a zero above means something).
  await page.evaluate(() => toast('probe'));
  expect(await toastMutations(page)).toBeGreaterThan(0);
});

// A poll that brings nothing new must not touch the banner: appendChild MOVES a node
// (remove, then insert), which drops the focus of a keyboard user on a line, clears a
// selection in a reason and makes a polite live region announce again every 3 s.
async function watchBanner(page) {
  await page.evaluate(() => {
    window.__bannerMutations = 0;
    window.__bannerPolls = 0;
    const b = document.getElementById('degradedBar');
    new MutationObserver((rs) => { window.__bannerMutations += rs.length; })
      .observe(b, { attributes: true, childList: true, characterData: true, subtree: true });
    const orig = window._degradedBanner;   // applyState calls it by its global name on every poll
    window._degradedBanner = function (g) { window.__bannerPolls++; return orig.apply(this, arguments); };
  });
}
async function pollBanner(page, n) {
  const before = await page.evaluate(() => window.__bannerPolls);
  for (let i = 0; i < n; i++) await page.evaluate(() => loadState());
  await page.waitForFunction((want) => window.__bannerPolls >= want, before + n);
  return page.evaluate(() => new Promise((res) => setTimeout(() => res(window.__bannerMutations), 50)));
}

test('a poll with nothing new leaves the banner untouched: no DOM change, focus and selection kept', async ({ page }) => {
  await seedToken(page);
  const dev = { degraded: degraded(FALLBACK, PARKED, CLOCK) };
  await routeState(page, dev);
  await openApp(page);
  await expect(bar(page).locator('details')).toHaveCount(3);
  const second = bar(page).locator('details summary').nth(1);
  await second.focus();
  await expect(second).toBeFocused();
  // Open the third and select a few words of its reason, as someone copying it would.
  const third = bar(page).locator('details').nth(2);
  await third.locator('summary').click();
  await expect(third.locator('p')).toBeVisible();
  await second.focus();
  await page.evaluate(() => {
    const p = document.querySelectorAll('#degradedBar details')[2].querySelector('p');
    const r = document.createRange();
    r.setStart(p.firstChild, 4); r.setEnd(p.firstChild, 20);
    const sel = window.getSelection(); sel.removeAllRanges(); sel.addRange(r);
    window.__selected = sel.toString();
  });
  await watchBanner(page);
  expect(await pollBanner(page, 3)).toBe(0);              // three unchanged polls: not one node touched
  await expect(second).toBeFocused();                      // the keyboard user is still on the line
  expect(await page.evaluate(() => window.getSelection().toString())).toBe(await page.evaluate(() => window.__selected));
  await expect(third.locator('p')).toBeVisible();          // and the open reason is still open
  // A reason that changes in one row touches that row's text only: focus still stays.
  dev.degraded = degraded(FALLBACK, { ...PARKED, why: 'A reason that moved.' }, CLOCK);
  await page.evaluate(() => { window.__bannerMutations = 0; });
  const changed = await pollBanner(page, 1);
  expect(changed).toBeGreaterThan(0);                      // the detector is live
  await expect(bar(page).locator('details').nth(1).locator('p')).toHaveText('A reason that moved.');
  await expect(second).toBeFocused();
});
