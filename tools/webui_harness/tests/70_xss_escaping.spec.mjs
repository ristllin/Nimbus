// DOM/stored-XSS regression guards for two sinks that reach the durable web-auth token
// (localStorage nimbusTok, auto-sent as X-Nimbus-Token, so an injected script drives every
// /api call as owner). Both interpolate attacker/model-controlled strings into innerHTML
// and are now HTML-escaped (_sEsc) before the sink:
//   F1 - the Telegram pending-approval display name (a stranger's Telegram first name).
//   F4 - the active-sessions table spawn fields (free-form, injection-steerable LLM output).
// If a regression drops the escaping, the injected <img onerror> becomes a real element and
// fires, flipping window.__xssFired; these specs assert it never fires and no element is made.
import { test, expect } from '@playwright/test';
import { seedToken, openApp } from './_helpers.mjs';
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
