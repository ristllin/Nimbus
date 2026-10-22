// Code sandbox and connector cards (wf_plan 4a-4c, the 22 Oct connector honesty items).
//
// The Code sandbox switch under Tools is the only control for the hosted
// code_interpreter: no Connectors card for it (Lumi's Mistral card hid OpenAI's
// sandbox), a status line from the device's one PDF rule, switches that survive the
// 5 s poll, a "?" that opens its own hint, a provider switch that redraws a card, a
// late voice list that cannot overwrite a newer pick, and the Mistral card's honest
// workspace line with Check Now.
import { test, expect } from '@playwright/test';
import { seedToken, openApp } from './_helpers.mjs';
import { ORCH, CONNECTORS } from '../fixtures.mjs';

const KNOWN = [
  { id: 'gcal', name: 'Google Calendar', providers: 'openai,mistral', kind: 'connector', cid: 'connector_googlecalendar', mcid: 'google_calendar', cred: 'OAuth', desc: 'Calendar.', docs: '' },
  { id: 'github', name: 'GitHub', providers: 'openai,anthropic,mistral', kind: 'mcp', cid: '', mcid: 'github_app', cred: 'GitHub PAT', desc: 'Repos.', docs: '' },
  { id: 'code_interpreter', name: 'Code Interpreter (hosted Python)', providers: 'mistral,openai', kind: 'builtin', cid: '', cred: 'None', desc: 'Hosted Python.', docs: '' },
];

function connectors(extra = {}) {
  return {
    configured: [
      { name: 'code_interpreter', type: 'code_interpreter', prov: 'mistral', kind: 'builtin', en: 1 },
      { name: 'gcal', type: 'gcal', prov: 'mistral', kind: 'connector', en: 1, auth: 2, ws: 'unchecked' },
    ],
    known: KNOWN, keyed: { mistral: true, openai: true, anthropic: true }, host: 'anthropic',
    wsChecking: false, wsMiss: 'memory', wsMissAgeS: 40, wsRecheckS: 1500, mistralTools: 1, mistralToolsAdvised: 2,
    ...extra,
  };
}

async function routeJson(page, url, body, onPost) {
  await page.route(url, (r) => {
    if (r.request().method() === 'POST' && onPost) return onPost(r);
    return r.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(typeof body === 'function' ? body() : body) });
  });
}

async function openSub(page, sp) {
  await openApp(page);
  await page.locator('.tab[data-p=assistant]').click();
  await page.locator(`.subtab[data-sp=${sp}]`).click();
}

async function openToolUse(page) {
  await openSub(page, 'tools');
  const grp = page.locator('#subpane-tools details.setgroup', { hasText: 'Tool use' }).first();
  if (!(await grp.evaluate((d) => d.open))) await grp.locator('summary').click();
  await expect(page.locator('#codeSbx')).toBeVisible();
}

test('a stored Code Interpreter card gets no Connectors card, and no URL or token fields', async ({ page }) => {
  await seedToken(page);
  await routeJson(page, '**/api/connectors', connectors());
  await openSub(page, 'connectors');
  await expect(page.locator('#cc_gcal_st')).toBeAttached();
  await expect(page.locator('#cc_code_interpreter_tile')).toHaveCount(0);
  await expect(page.locator('#cc_code_interpreter_url')).toHaveCount(0);
  await expect(page.locator('#cc_code_interpreter_tok')).toHaveCount(0);
  await expect(page.locator('#conncards')).not.toContainText('Code Interpreter');
});

test('the Code sandbox status line follows the device PDF route', async ({ page }) => {
  await seedToken(page);
  const orch = { ...ORCH, codeSbx: 1, pdfRoute: 'openai' };
  await routeJson(page, '**/api/orch', () => orch, (r) => r.fulfill({ status: 200, contentType: 'application/json', body: '{"ok":true}' }));
  await openToolUse(page);
  await expect(page.locator('#codeSbxStat')).toHaveText('Ready. PDFs go through OpenAI.');
  orch.pdfRoute = 'nostore';
  await page.evaluate(() => loadOrch());
  await expect(page.locator('#codeSbxStat')).toHaveText('Insert an SD card to keep files.');
  orch.pdfRoute = 'device';
  await page.evaluate(() => loadOrch());
  await expect(page.locator('#codeSbxStat')).toContainText('made on the device');
});

test('a switch flipped mid-save survives two polls, and saves on its own', async ({ page }) => {
  await seedToken(page);
  const posts = [];
  let release;
  const held = new Promise((res) => { release = res; });
  const orch = { ...ORCH, codeSbx: 0, orchTrace: 1, pdfRoute: 'nokey' };
  await routeJson(page, '**/api/orch', () => orch, async (r) => {
    posts.push(r.request().postData() || '');
    await held;   // the save is slow: two polls run meanwhile
    return r.fulfill({ status: 200, contentType: 'application/json', body: '{"ok":true}' });
  });
  await openToolUse(page);
  await expect(page.locator('#codeSbx')).not.toBeChecked();
  await page.locator('#codeSbx').check();
  await page.locator('#orchTrace').uncheck();
  await page.evaluate(() => { loadOrch(); });
  await page.waitForTimeout(300);
  await page.evaluate(() => { loadOrch(); });
  await page.waitForTimeout(300);
  await expect(page.locator('#codeSbx')).toBeChecked();
  await expect(page.locator('#orchTrace')).not.toBeChecked();
  expect(posts.some((p) => /codeSbx=1/.test(p))).toBe(true);
  expect(posts.some((p) => /orchTrace=0/.test(p))).toBe(true);
  // Each switch posts only itself (no Telegram token, no provider field).
  expect(posts.every((p) => !/tgToken|orchHost|custKey/.test(p))).toBe(true);
  orch.codeSbx = 1; orch.orchTrace = 0;
  release();
  await page.waitForTimeout(400);
  await page.evaluate(() => { loadOrch(); });
  await expect(page.locator('#codeSbx')).toBeChecked();
});

test('the Primary provider saves on change, without the Save button', async ({ page }) => {
  await seedToken(page);
  const posts = [];
  await routeJson(page, '**/api/orch', () => ({ ...ORCH, orchHost: '' }), (r) => {
    posts.push(r.request().postData() || '');
    return r.fulfill({ status: 200, contentType: 'application/json', body: '{"ok":true}' });
  });
  await openSub(page, 'llm');
  await page.locator('#subpane-llm details.setgroup summary', { hasText: 'Routing' }).click();
  await page.locator('#orchHost').selectOption('mistral');
  await expect.poll(() => posts.join('|')).toContain('orchHost=mistral');
  expect(posts.every((p) => !/tgToken/.test(p))).toBe(true);
});

test('the "?" on Enabled opens the tool use hint, and the Code sandbox "?" its own', async ({ page }) => {
  await seedToken(page);
  await openToolUse(page);
  const toolQ = page.locator('label:has(#orchLoop) button.qh');
  await toolQ.click();
  const toolTip = page.locator('#subpane-tools p.tip', { hasText: 'When on, the assistant uses its tools mid-turn' });
  await expect(toolTip).toHaveClass(/open/);
  await expect(page.locator('#subpane-tools p.tip', { hasText: 'run Python on OpenAI or Mistral' })).not.toHaveClass(/open/);
  await page.locator('#codeSbxRow button.qh').click();
  await expect(page.locator('#subpane-tools p.tip', { hasText: 'run Python on OpenAI or Mistral' })).toHaveClass(/open/);
});

test('switching a card provider redraws it: hint, fields and the provider id', async ({ page }) => {
  await seedToken(page);
  await routeJson(page, '**/api/connectors', connectors({ configured: [] }));
  await openSub(page, 'connectors');
  // GitHub as an OpenAI remote MCP: URL and token fields, no Mistral hint.
  await page.locator('#cc_github_prov').selectOption('openai');
  await expect(page.locator('#cc_github_urlrow')).toBeVisible();
  await expect(page.locator('#cc_github_tokrow')).toBeVisible();
  await page.locator('#cc_github_tok').fill('typed-token');
  // On Mistral it is a Studio connector: no URL, no token, Mistral's own id, the hint.
  await page.locator('#cc_github_prov').selectOption('mistral');
  await expect(page.locator('#cc_github_urlrow')).toBeHidden();
  await expect(page.locator('#cc_github_tokrow')).toBeHidden();
  await expect(page.locator('#cc_github_cid')).toHaveValue('github_app');
  await expect(page.locator('#cc_github_tile')).toContainText('turn GitHub on in Mistral Studio');
  await expect(page.locator('#cc_github_st')).toContainText('not saved');
  // Google Calendar: the Mistral id on Mistral, the OpenAI id on OpenAI.
  await page.locator('#cc_gcal_prov').selectOption('mistral');
  await expect(page.locator('#cc_gcal_cid')).toHaveValue('google_calendar');
  await page.locator('#cc_gcal_prov').selectOption('openai');
  await expect(page.locator('#cc_gcal_cid')).toHaveValue('connector_googlecalendar');
});

test('a Save on one card keeps the unsaved edits on another', async ({ page }) => {
  await seedToken(page);
  await routeJson(page, '**/api/connectors', connectors({ configured: [] }),
    (r) => r.fulfill({ status: 200, contentType: 'application/json', body: '{"ok":true}' }));
  await openSub(page, 'connectors');
  await page.locator('#cc_github_prov').selectOption('anthropic');
  await page.locator('#cc_github_url').fill('https://mcp.example.dev/');
  await page.locator('#cc_gcal_en').check();
  await page.locator('#cc_gcal_tile button', { hasText: 'Save' }).click();
  await page.waitForTimeout(500);
  await expect(page.locator('#cc_github_prov')).toHaveValue('anthropic');
  await expect(page.locator('#cc_github_url')).toHaveValue('https://mcp.example.dev/');
  await expect(page.locator('#cc_github_st')).toContainText('not saved');
});

test('an unchecked Mistral connector says why, when it is checked next, and offers Check Now', async ({ page }) => {
  await seedToken(page);
  const checks = [];
  await page.route('**/api/connectors/check', (r) => {
    checks.push(r.request().method());
    return r.fulfill({ status: 200, contentType: 'application/json', body: '{"ok":true,"queued":true}' });
  });
  await routeJson(page, '**/api/connectors', connectors());
  await openSub(page, 'connectors');
  const line = page.locator('#cc_gcal_ws');
  await expect(line).toContainText('The last check did not finish: the device was low on memory.');
  await expect(line).toContainText('Next automatic check in 25 min.');
  await line.locator('button', { hasText: 'Check Now' }).click();
  await expect.poll(() => checks.length).toBe(1);
  expect(checks[0]).toBe('POST');
});

test('a connector the workspace does not offer says so, and too many Mistral tools warn', async ({ page }) => {
  await seedToken(page);
  const body = connectors({ wsMiss: undefined, wsRecheckS: undefined, mistralTools: 4 });
  body.configured[1].ws = 'unlisted';
  await routeJson(page, '**/api/connectors', body);
  await openSub(page, 'connectors');
  await expect(page.locator('#cc_gcal_ws')).toContainText('Your Mistral workspace does not offer Google Calendar yet');
  await expect(page.locator('#connwarn')).toBeVisible();
  await expect(page.locator('#connwarn')).toContainText('4 Mistral connectors and built-ins are on');
});

test('a late voice list for the previous provider never overwrites the newer pick', async ({ page }) => {
  await seedToken(page);
  await page.route('**/api/voices?provider=mistral', async (r) => {
    await new Promise((res) => setTimeout(res, 1500));
    return r.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify([
      { value: 'en_paul_neutral', label: 'Paul (male, US) - neutral', name: 'Paul', gender: 'male', lang: 'US', emotion: 'neutral' }]) });
  });
  await page.route('**/api/voices?provider=openai', (r) => r.fulfill({ status: 200, contentType: 'application/json',
    body: JSON.stringify([{ value: 'alloy', label: 'Alloy - neutral' }, { value: 'nova', label: 'Nova - female' }]) }));
  await openApp(page);
  await page.evaluate(() => { fillVoices('mistral', 'en_paul_neutral', false); fillVoices('openai', 'nova', false); });
  await page.waitForTimeout(2200);   // the Mistral list lands late
  const opts = await page.locator('#ttsVoice option').allTextContents();
  expect(opts).toContain('Nova - female');
  expect(opts.join()).not.toContain('Paul');
  await expect(page.locator('#ttsVoice')).toHaveValue('nova');
});

test('a fresh device: no connectors, sandbox off, the status says what to add', async ({ page }) => {
  await seedToken(page);
  await routeJson(page, '**/api/orch', { ...ORCH, codeSbx: 0, pdfRoute: 'nokey',
    providers: { cumulo: ORCH.providers.cumulo } });
  await routeJson(page, '**/api/connectors', { ...CONNECTORS, configured: [], keyed: {}, mistralTools: 0 });
  await openToolUse(page);
  await expect(page.locator('#codeSbx')).not.toBeChecked();
  await expect(page.locator('#codeSbxStat')).toHaveText('Add your OpenAI or Anthropic key to make PDFs.');
  await page.locator('.subtab[data-sp=connectors]').click();
  await expect(page.locator('#connwarn')).toBeHidden();
  await expect(page.locator('#conncards')).not.toContainText('Code Interpreter');
});

test('a hosted instance shows no Code sandbox switch (it cannot collect the files)', async ({ page }) => {
  await seedToken(page);
  await page.addInitScript(() => { window.NIMBUS_HOSTED = true; });
  await openToolUse(page).catch(() => {});
  await expect(page.locator('#codeSbxRow')).toBeHidden();
});
