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

test('calm: the banner never toasts or flashes, and a recovery says nothing', async ({ page }) => {
  await seedToken(page);
  const dev = { degraded: degraded() };
  await routeState(page, dev);
  await openApp(page);
  const toastsBefore = await page.locator('.toast').count();
  dev.degraded = degraded(FALLBACK, MEMORY);
  await page.evaluate(() => loadState());
  await expect(bar(page)).toBeVisible();
  dev.degraded = degraded();
  await page.evaluate(() => loadState());
  await expect(bar(page)).toBeHidden();
  expect(await page.locator('.toast').count()).toBe(toastsBefore);
});
