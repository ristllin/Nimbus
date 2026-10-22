// CUM-537 gate: while the device checks the Mistral workspace (wsChecking), the
// Connectors tab polls /api/connectors every 4 s so a badge turns to its real
// state without a reload. That poll used to rebuild every card, wiping a token
// being typed or a provider being picked. A poll now refreshes only the status
// badges, and it stops once polling is paused.
import { test, expect } from '@playwright/test';
import { seedToken, openApp } from './_helpers.mjs';

const KNOWN = [
  { id: 'gcal', name: 'Google Calendar', providers: 'openai,mistral', kind: 'connector', cid: '', cred: 'Google', desc: 'Calendar.', docs: '' },
  { id: 'github', name: 'GitHub', providers: 'openai,anthropic,mistral', kind: 'mcp', cid: '', cred: 'GitHub PAT', desc: 'Repos.', docs: '' },
];

function payload(wsChecking, ws, auth) {
  return {
    configured: [{ type: 'gcal', name: 'gcal', prov: 'mistral', kind: 'connector', en: 1, auth, ws }],
    known: KNOWN, keyed: { mistral: true, anthropic: true }, host: 'anthropic', wsChecking,
  };
}

async function openConnectors(page, state) {
  state.calls = 0;
  await page.route('**/api/connectors**', (r) => {
    state.calls++;
    return r.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(state.body) });
  });
  await openApp(page);
  await page.locator('.tab[data-p=assistant]').click();
  await page.locator('.subtab[data-sp=connectors]').click();
  await expect(page.locator('#cc_github_tok')).toBeAttached();
}

test('a workspace-check poll keeps a token being typed and the picked provider', async ({ page }) => {
  await seedToken(page);
  const state = { body: payload(true, 'checking', 2) };
  await openConnectors(page, state);
  await expect(page.locator('#conncards')).toContainText('checking your Mistral workspace');
  // The owner picks a provider on one card and starts typing a token on another.
  await page.locator('#cc_github_prov').selectOption('anthropic');
  const tok = page.locator('#cc_github_tok');
  await tok.click();
  await tok.pressSequentially('abc');
  // One real poll cycle (4 s timer) and one forced poll.
  const before = state.calls;
  await page.waitForTimeout(4600);
  expect(state.calls).toBeGreaterThan(before);
  await page.evaluate(() => loadConnectors(true));
  await expect(tok).toHaveValue('abc');
  await expect(page.locator('#cc_github_prov')).toHaveValue('anthropic');
  expect(await page.evaluate(() => document.activeElement && document.activeElement.id)).toBe('cc_github_tok');
  // The check lands: the badge turns to its real state, the edit is still there.
  state.body = payload(false, '', 1);
  await page.evaluate(() => loadConnectors(true));
  await expect(page.locator('#conncards')).toContainText('via sessions');
  await expect(tok).toHaveValue('abc');
});

test('a card that appears during the check never rebuilds the others, focused or not', async ({ page }) => {
  await seedToken(page);
  const state = { body: payload(true, 'checking', 2) };
  await openConnectors(page, state);
  // A token typed, then focus moved away (a phone select drops focus the same way).
  await page.locator('#cc_github_tok').fill('typed');
  await page.evaluate(() => document.activeElement && document.activeElement.blur());
  expect(await page.evaluate(() => document.activeElement === document.body)).toBe(true);
  // Another tab adds a custom connector while the check runs.
  state.body = payload(true, 'checking', 2);
  state.body.configured.push({ name: 'crm', type: 'crm', prov: 'mistral', kind: 'connector', en: 1, auth: 2, ws: 'checking' });
  await page.evaluate(() => loadConnectors(true));
  await page.waitForTimeout(300);
  await expect(page.locator('#cc_github_tok')).toHaveValue('typed');
  await expect(page.locator('#cc_crm_st')).toHaveCount(0);   // waits for the next full load
  await page.evaluate(() => loadConnectors());
  await expect(page.locator('#cc_crm_st')).toHaveCount(1);
});

test('the poll stops once polling is paused', async ({ page }) => {
  await seedToken(page);
  const state = { body: payload(true, 'checking', 2) };
  await openConnectors(page, state);
  await page.evaluate(() => { _authPaused = true; });
  const before = state.calls;
  await page.evaluate(() => loadConnectors(true));
  await page.waitForTimeout(4600);
  expect(state.calls).toBe(before);
});

test('an explicit load (tab open, save) renders the cards in full and keeps an unsaved card', async ({ page }) => {
  await seedToken(page);
  const state = { body: payload(false, '', 1) };
  await openConnectors(page, state);
  await page.locator('#cc_github_tok').fill('typed');
  await page.evaluate(() => loadConnectors());
  // wf_plan 4c: a full rebuild (another card's Save or Remove) keeps what the owner
  // typed on a card they have not saved, and says it is not saved yet.
  await expect(page.locator('#cc_github_tok')).toHaveValue('typed');
  await expect(page.locator('#cc_github_st')).toContainText('not saved');
  // A card nobody touched renders fresh from the device.
  await expect(page.locator('#cc_gcal_st')).toContainText('via sessions');
});
