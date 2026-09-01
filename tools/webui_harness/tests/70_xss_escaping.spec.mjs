// DOM/stored-XSS regression guards for sinks that reach the durable web-auth token
// (localStorage nimbusTok, auto-sent as X-Nimbus-Token, so an injected script drives every
// /api call as owner). Each interpolates an attacker/model-controlled string into innerHTML
// and is now HTML-escaped before the sink:
//   F1 - the Telegram pending-approval display name (a stranger's Telegram first name), _sEsc.
//   F4 - the active-sessions table spawn fields (free-form, injection-steerable LLM output), _sEsc.
//   CUM-281 F3 - the memory dashboard error line renders d.error (an embedding-provider error
//     string) into #memlist innerHTML; now escaped with _wesc at the sink.
// If a regression drops the escaping, the injected <img onerror> becomes a real element and
// fires, flipping window.__xssFired; these specs assert it never fires and no element is made.
import { test, expect } from '@playwright/test';
import { seedToken, openApp, assertPane } from './_helpers.mjs';
import { ORCH } from '../fixtures.mjs';

// onerror flips a global the moment the browser fails to load src=x on a REAL <img> element.
// When the value is correctly escaped, no element exists and the flag stays unset.
const IMG_PAYLOAD = '<img src=x onerror="window.__xssFired=1">';

test('F1: a Telegram pending name with an HTML payload is escaped, not executed', async ({ page }) => {
  await seedToken(page);
  await page.route('**/api/telegram', (route) =>
    route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: JSON.stringify({
        enabled: true,
        public: false,
        allow: [],
        pending: [{ chatId: '<b>evilchat</b>', name: IMG_PAYLOAD, preview: 'hey <there>' }],
      }),
    }));
  await openApp(page);

  const pending = page.locator('#tgPending');
  // The row's static "wants access" text renders whether or not the payload is escaped.
  await expect(pending).toContainText('wants access', { timeout: 10_000 });
  // No element was ever synthesized from the name or the chatId (both escaped).
  await expect(pending.locator('img')).toHaveCount(0);
  await expect(pending.locator('b', { hasText: 'evilchat' })).toHaveCount(0);
  // The escaped payload shows as literal text.
  await expect(pending).toContainText('<img src=x onerror');
  // The onerror never ran.
  expect(await page.evaluate(() => window.__xssFired)).toBeUndefined();
});

test('F4: an active-session field with an HTML payload is escaped, not executed', async ({ page }) => {
  await seedToken(page);
  // Base on the known-good ORCH fixture so applyOrch runs clean down to the jobs block,
  // then poison one spawn field (category) with the payload.
  const poisoned = {
    ...ORCH,
    jobs: [{ tag: 'sess-x', backend: 'anthropic', model: 'claude-sonnet-5', category: IMG_PAYLOAD, state: 'running' }],
  };
  await page.route('**/api/orch', (route) =>
    route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(poisoned) }));
  await openApp(page);

  const dash = page.locator('#dashJobs');
  // The row's safe tag renders in both escaped and unescaped worlds.
  await expect(dash).toContainText('sess-x', { timeout: 10_000 });
  // No element was synthesized from the poisoned category.
  await expect(dash.locator('img')).toHaveCount(0);
  await expect(dash).toContainText('<img src=x onerror');
  expect(await page.evaluate(() => window.__xssFired)).toBeUndefined();
});

test('F3: a memory embedding-error string with an HTML payload is escaped, not executed', async ({ page }) => {
  await seedToken(page);
  // The memory list lazy-loads from /api/mem/vector; an embedding-provider failure comes
  // back as { error } with no entries, and renderMemList() drops it into #memlist innerHTML.
  // Poison that error string; the empty entries[] forces the error branch (the sink).
  await page.route('**/api/mem/vector**', (route) =>
    route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: JSON.stringify({ entries: [], total: 0, error: IMG_PAYLOAD }),
    }));
  await openApp(page);
  await page.locator('.tab[data-p=memory]').click();
  await assertPane(page, 'memory');

  const memlist = page.locator('#memlist');
  // The static "Error:" prefix renders whether or not the payload is escaped.
  await expect(memlist).toContainText('Error:', { timeout: 10_000 });
  // No element was synthesized from the error string (escaped at the sink).
  await expect(memlist.locator('img')).toHaveCount(0);
  // The escaped payload shows as literal text.
  await expect(memlist).toContainText('<img src=x onerror');
  // The onerror never ran.
  expect(await page.evaluate(() => window.__xssFired)).toBeUndefined();
});
