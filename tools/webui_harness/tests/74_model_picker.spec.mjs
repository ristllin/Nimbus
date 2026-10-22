// CUM-26: the model picker renders the FULL catalog from GET /api/models, not the
// 8-cap `choices` CSV. Expectations: a "Recommended" group on top (from choices),
// then the full list grouped - by UPSTREAM for cumulo (its <upstream>/<model>
// router shape), by SIZE CLASS for every other provider - flagship-first, with a
// size badge in each option label, and non-chat models (embeddings) filtered out.
// Cold start (no catalog yet) still falls back to the flat choices list unchanged.
//
// T4 (host) tier: overrides /api/orch (rows + choices) and /api/models (catalog).
// Runs unchanged on a real device over LAN (T5/HIL) - only the mock overrides differ.
import { test, expect } from '@playwright/test';
import { seedToken, openApp } from './_helpers.mjs';

function orchPayload() {
  const prov = (hasKey, verify, choices) => ({
    hasKey, verify, vts: verify === -1 ? 0 : 1700000000,
    orchModel: '', subModel: '', choices,
  });
  return {
    running: true,
    providers: {
      cumulo: prov(true, 1, 'openai/gpt-5.5,anthropic/claude-opus-5'),
      openai: prov(true, 1, ''),   // no recommended set: isolates the size grouping below
      anthropic: prov(true, 1, 'claude-opus-5,claude-sonnet-5'),
      mistral: prov(false, -1, ''),
      zai: prov(false, -1, ''),
    },
    cust: { base: '', conv: 'openai', model: '', hasKey: false },
    orchHost: 'anthropic',
    provPrio: 'cumulo,anthropic', subPrio: 'cumulo,anthropic',
    sttProv: 'openai', ttsOn: false, ttsProv: 'openai', ttsVoice: '',
    theme: 'nimbus', hasTav: false, hasTg: false,
    tgAllow: '', tgBot: '', tgLive: false, tgVerify: -1, tgVts: 0,
    tavVerify: -1, tavVts: 0,
    fetchPol: 'ask', compactKB: 0, loopRounds: 0, loopDeadline: '', orchLoop: false,
    orchTrace: false, midFail: 0, tlsSlots: 1, tlsVerify: 1,
    modInbound: 0, modOutbound: 0, modInjection: 0,
    sfxLvlN: 2, sfxLvlO: 2, sfxTheme: 'terran', sfxVol: 60, sfxTier: 'basic', sfxSync: 'idle',
    usage: { sessIn: 0, sessOut: 0, turns: 0, lastIn: 0, lastOut: 0, byProvider: [] },
    jobs: [],
  };
}

const cm = (id, size, roles = ['orchestrator', 'sub-agent'], extra = {}) =>
  ({ id, roles, usable: true, probed: true, size, family: '', source: 'api', deprecated: false, ...extra });

function modelsPayload() {
  return {
    generatedAt: 1700000000, ttlSec: 86400,
    roles: ['orchestrator', 'sub-agent', 'embedding', 'vision', 'stt', 'tts', 'image'],
    providers: {
      openai: {
        keyed: true, verified: 1, probe: 1, refreshedAt: 1700000000, stale: false,
        models: [
          cm('gpt-6-astra', 'L'), cm('gpt-5.5', 'L'), cm('gpt-5.4-mini', 'S'),
          cm('gpt-4o', 'M', ['orchestrator', 'sub-agent'], { deprecated: true }),
          cm('text-embedding-3-large', '', ['embedding']), // must NOT appear in the chat picker
        ],
      },
      anthropic: {
        keyed: true, verified: 1, probe: 1, refreshedAt: 1700000000, stale: false,
        models: [cm('claude-opus-5', 'L'), cm('claude-sonnet-5', 'M'), cm('claude-haiku-4-5', 'S')],
      },
      cumulo: {
        keyed: true, verified: 1, probe: 1, refreshedAt: 1700000000, stale: false,
        models: [
          cm('gpt-5.5', 'L', ['orchestrator', 'sub-agent'], { upstream: 'openai' }),
          cm('gpt-5.4-mini', 'S', ['orchestrator', 'sub-agent'], { upstream: 'openai' }),
          cm('claude-opus-5', 'L', ['orchestrator', 'sub-agent'], { upstream: 'anthropic' }),
          cm('claude-haiku-4-5', 'S', ['orchestrator', 'sub-agent'], { upstream: 'anthropic' }),
          cm('glm-4.6', 'M', ['orchestrator', 'sub-agent'], { upstream: 'zai' }),
        ],
      },
      zai: { keyed: false, verified: -1, probe: 1, refreshedAt: 0, stale: true, models: [] },
    },
  };
}

async function routeAll(page) {
  await page.route('**/api/orch', (route) => {
    if (route.request().method() === 'GET')
      return route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(orchPayload()) });
    return route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ ok: true }) });
  });
  await page.route('**/api/models', (route) =>
    route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(modelsPayload()) }));
}

async function openModels(page) {
  await seedToken(page);
  await routeAll(page);
  await openApp(page);
  await page.locator('.tab[data-p=assistant]').click();
  await page.locator('.subtab[data-sp=llm]').click();
  await page.locator('#subpane-llm details.setgroup').first().locator('summary').click();
  await expect(page.locator('#provs .provrow').first()).toBeVisible();
}

// Group labels visible inside a <select>, in DOM order.
async function optgroupLabels(page, selId) {
  return page.locator(`#${selId} optgroup`).evaluateAll((gs) => gs.map((g) => g.label));
}

test('cumulo picker: Recommended on top, then groups by upstream, with size badges', async ({ page }, testInfo) => {
  await openModels(page);
  const sel = page.locator('#orchM_cumulo');
  await expect(sel).toBeEnabled();
  // Recommended is the FIRST optgroup; the three upstream groups follow.
  const labels = await optgroupLabels(page, 'orchM_cumulo');
  expect(labels[0]).toBe('Recommended');
  expect(labels).toContain('OpenAI');
  expect(labels).toContain('Anthropic');
  expect(labels).toContain('Z.ai');
  // The full catalog is more than the two-id `choices` list a device would show
  // pre-harvest: every upstream model is present, grouped.
  const optCount = await sel.locator('option').count();
  expect(optCount).toBeGreaterThan(3);
  await expect(sel.locator('optgroup')).toHaveCount(4);
  // Size badge rendered in an option label (L for a flagship).
  await expect(sel.locator('option', { hasText: '· L' }).first()).toHaveCount(1);
  // The full router id is the option VALUE (what the device stores).
  await expect(sel.locator('option[value="anthropic/claude-opus-5"]')).toHaveCount(1);
  await page.screenshot({ path: `screenshots/cum26-picker-cumulo-${testInfo.project.name}.png`, fullPage: true });
  // A native <select> popup cannot be screenshotted, so dump the grouped option
  // structure as an inspectable text artifact (the "full catalog, grouped" evidence).
  const dump = await sel.evaluate((s) => Array.from(s.children).map((c) => {
    if (c.tagName === 'OPTGROUP')
      return `[${c.label}]\n` + Array.from(c.children).map((o) => `   - ${o.textContent}  (value=${o.value})`).join('\n');
    return `${c.textContent}  (value=${c.value})`;
  }).join('\n'));
  const fs = await import('node:fs');
  fs.writeFileSync(`screenshots/cum26-picker-cumulo-options-${testInfo.project.name}.txt`,
    'orchM_cumulo dropdown structure (Recommended shortcut + full catalog grouped by upstream):\n\n' + dump + '\n');
});

// CUM-530: a router row's option VALUE is its selector "<upstream>/<id>" - the
// device catalog keeps the bare id + upstream, and the grouped rows used to post
// the bare id, which the device's save check refused (silently, before CUM-530).
test('cumulo picker: every grouped row posts its <upstream>/<id> selector', async ({ page }) => {
  let posted = null;
  await seedToken(page);
  await page.route('**/api/orch', (route) => {
    if (route.request().method() === 'GET')
      return route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(orchPayload()) });
    posted = new URLSearchParams(route.request().postData() || '');
    return route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ ok: true }) });
  });
  await page.route('**/api/models', (route) =>
    route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(modelsPayload()) }));
  await openApp(page);
  await page.locator('.tab[data-p=assistant]').click();
  await page.locator('.subtab[data-sp=llm]').click();
  await page.locator('#subpane-llm details.setgroup').first().locator('summary').click();
  const sel = page.locator('#orchM_cumulo');
  await expect(sel).toBeEnabled();
  // A catalog-only row (not in Recommended) carries the full selector...
  await expect(sel.locator('option[value="anthropic/claude-haiku-4-5"]')).toHaveCount(1);
  await expect(sel.locator('option[value="claude-haiku-4-5"]')).toHaveCount(0);
  await expect(sel.locator('option[value="zai/glm-4.6"]')).toHaveCount(1);
  // ...and picking it posts that selector.
  await sel.selectOption('anthropic/claude-haiku-4-5');
  await expect.poll(() => posted && posted.get('orchM_cumulo')).toBe('anthropic/claude-haiku-4-5');
});

// CUM-530: a refused pick answers 400 with the allowed values; the page shows the
// device's sentence under the provider row instead of a silent "Saved".
test('cumulo picker: a refused model shows the device reason under the row', async ({ page }) => {
  const reason = "That model isn't available for Cumulo Nimbus. Choose one of: openai/gpt-5.5, anthropic/claude-opus-5.";
  await seedToken(page);
  await page.route('**/api/orch', (route) => {
    if (route.request().method() === 'GET')
      return route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(orchPayload()) });
    return route.fulfill({ status: 400, contentType: 'application/json', body: JSON.stringify({ error: reason }) });
  });
  await page.route('**/api/models', (route) =>
    route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(modelsPayload()) }));
  await openApp(page);
  await page.locator('.tab[data-p=assistant]').click();
  await page.locator('.subtab[data-sp=llm]').click();
  await page.locator('#subpane-llm details.setgroup').first().locator('summary').click();
  await page.locator('#orchM_cumulo').selectOption('zai/glm-4.6');
  await expect(page.locator('#pmsg_cumulo')).toHaveText(reason);
  // prism: the select goes back to the stored model (the default here), never
  // keeps showing the refused one.
  await expect(page.locator('#orchM_cumulo')).toHaveValue('');
});

// prism: before the first harvest the router's cold-start list is bare ids; a
// stored bare default and the list entry are ONE row, never two.
test('cumulo picker: cold start shows a stored default once', async ({ page }) => {
  await seedToken(page);
  const orch = orchPayload();
  orch.providers.cumulo.choices = 'gpt-5.6,gpt-5.6-terra';
  orch.providers.cumulo.orchModel = 'gpt-5.6';
  await page.route('**/api/orch', (route) =>
    route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(orch) }));
  await page.route('**/api/models', (route) =>
    route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ generatedAt: 0, providers: {} }) }));
  await openApp(page);
  await page.locator('.tab[data-p=assistant]').click();
  await page.locator('.subtab[data-sp=llm]').click();
  await page.locator('#subpane-llm details.setgroup').first().locator('summary').click();
  const sel = page.locator('#orchM_cumulo');
  await expect(sel.locator('option', { hasText: /^openai\/gpt-5\.6$|^gpt-5\.6$/ })).toHaveCount(1);
  await expect(sel).toHaveValue('openai/gpt-5.6');
});

// CUM-530: a catalog cut to its size bound says so ("<n> more not shown").
test('cumulo picker: a trimmed catalog says how many models are not shown', async ({ page }) => {
  await seedToken(page);
  await page.route('**/api/orch', (route) =>
    route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(orchPayload()) }));
  await page.route('**/api/models', (route) => {
    const m = modelsPayload();
    const rows = m.providers.cumulo.models;
    rows[rows.length - 1].more = 7;   // the device marks the last kept row
    return route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(m) });
  });
  await openApp(page);
  await page.locator('.tab[data-p=assistant]').click();
  await page.locator('.subtab[data-sp=llm]').click();
  await page.locator('#subpane-llm details.setgroup').first().locator('summary').click();
  await expect(page.locator('#orchM_cumulo option', { hasText: '7 more not shown (list truncated)' })).toHaveCount(1);
});

test('openai picker: grouped by size class, flagship-first, embeddings excluded', async ({ page }) => {
  await openModels(page);
  const sel = page.locator('#orchM_openai');
  await expect(sel).toBeEnabled();
  // No recommended set for openai here, so the dropdown is purely the size groups.
  const labels = await optgroupLabels(page, 'orchM_openai');
  // Flagship-first: Large precedes Medium precedes Small.
  const li = labels.indexOf('Large'), mi = labels.indexOf('Medium'), si = labels.indexOf('Small');
  expect(li).toBeGreaterThan(-1);
  expect(mi).toBeGreaterThan(li);
  expect(si).toBeGreaterThan(mi);
  // The embedding-only model is filtered out of the chat picker.
  await expect(sel.locator('option[value="text-embedding-3-large"]')).toHaveCount(0);
  // A deprecated chat model is still listed, marked as such.
  await expect(sel.locator('option', { hasText: 'deprecated' })).toHaveCount(1);
});

test('cold start (no catalog) falls back to the flat choices list', async ({ page }) => {
  await seedToken(page);
  await page.route('**/api/orch', (route) => {
    if (route.request().method() === 'GET')
      return route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(orchPayload()) });
    return route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ ok: true }) });
  });
  // Empty catalog: providers present but no models yet (pre-harvest).
  await page.route('**/api/models', (route) =>
    route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ generatedAt: 0, providers: {} }) }));
  await openApp(page);
  await page.locator('.tab[data-p=assistant]').click();
  await page.locator('.subtab[data-sp=llm]').click();
  await page.locator('#subpane-llm details.setgroup').first().locator('summary').click();
  // anthropic carries a static `choices` list here (openai's is empty in this payload).
  const sel = page.locator('#orchM_anthropic');
  await expect(sel).toBeEnabled();
  // No optgroups (flat list); the two static choices plus (default) are selectable.
  await expect(sel.locator('optgroup')).toHaveCount(0);
  await expect(sel.locator('option[value="claude-opus-5"]')).toHaveCount(1);
  await expect(sel.locator('option[value="claude-sonnet-5"]')).toHaveCount(1);
});

// R5: opening a model dropdown shows the cached list at once, asks the device for
// a fresh one (POST /api/models/refresh, rate-limited per provider on the device)
// and then says "Updated <time>". Host: the device answers are mocked; the first
// GET is the cached list, the refresh lands on a later GET. With TARGET=device the
// real device answers (the bench leg) and the page must end on "Updated".
const ON_DEVICE = process.env.TARGET === 'device';

function cachedThenFresh() {
  const now = Math.floor(Date.now() / 1000);
  const old = now - 3 * 86400;
  const anthropic = (models, fetchedAt, refreshPending) => ({
    keyed: true, verified: 1, probe: 1, refreshedAt: 0, stale: false, fetchedAt, refreshPending, models,
  });
  const cached = [cm('claude-opus-5', 'L'), cm('claude-sonnet-5', 'M')];
  const fresh = [cm('claude-opus-5-5', 'L'), cm('claude-sonnet-5-5', 'M'), cm('claude-haiku-5-5', 'S')];
  const body = (prov) => ({ generatedAt: now, ttlSec: 86400, roles: ['orchestrator', 'sub-agent'], providers: { anthropic: prov } });
  return {
    old, now,
    cachedBody: body(anthropic(cached, old, false)),
    pendingBody: body(anthropic(cached, old, true)),
    freshBody: body(anthropic(fresh, now, false)),
  };
}

test('model dropdown: cached list first, then a refresh and "Updated <time>"', async ({ page }, testInfo) => {
  test.setTimeout(ON_DEVICE ? 150000 : 30000);
  await seedToken(page);
  const s = cachedThenFresh();
  let posts = 0, gets = 0;
  if (!ON_DEVICE) {
    await page.route('**/api/orch', (route) =>
      route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(orchPayload()) }));
    await page.route(/\/api\/models(\?.*)?$/, (route) => {
      gets++;
      const b = posts === 0 ? s.cachedBody : (gets <= 2 ? s.pendingBody : s.freshBody);
      return route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(b) });
    });
    await page.route('**/api/models/refresh', (route) => {
      posts++;
      expect(new URLSearchParams(route.request().postData() || '').get('provider')).toBe('anthropic');
      return route.fulfill({ status: 200, contentType: 'application/json',
        body: JSON.stringify({ ok: true, provider: 'anthropic', refresh: 'queued', fetchedAt: s.old }) });
    });
  }
  await openApp(page);
  await page.locator('.tab[data-p=assistant]').click();
  await page.locator('.subtab[data-sp=llm]').click();
  const grp = page.locator('#subpane-llm details.setgroup').first();
  if (!(await grp.evaluate((d) => d.open))) await grp.locator('summary').click();
  const sel = page.locator('#orchM_anthropic');
  await expect(sel).toBeEnabled({ timeout: ON_DEVICE ? 20000 : 5000 });
  const upd = page.locator('#mupd_anthropic');
  if (!ON_DEVICE) {
    // The cached list is there at once, with its own age, before anything is asked.
    await expect(sel.locator('option[value="claude-opus-5"]')).toHaveCount(1);
    await expect(upd).toHaveText(/^Updated /);
    expect(posts).toBe(0);
  }
  await page.screenshot({ path: `screenshots/laneH-picker-cached-${testInfo.project.name}.png`, fullPage: true });
  await sel.focus();                                       // opening the dropdown
  if (!ON_DEVICE) {
    await expect(upd).toHaveText('Checking for new models');
    await expect.poll(() => posts).toBe(1);
  }
  await expect(upd).toHaveText(/^Updated \d/, { timeout: ON_DEVICE ? 120000 : 15000 });
  await page.screenshot({ path: `screenshots/laneH-picker-updated-${testInfo.project.name}.png`, fullPage: true });
  await sel.blur();
  if (!ON_DEVICE) {
    // The fresh list replaces the cached one once the select is closed.
    await expect(sel.locator('option[value="claude-sonnet-5-5"]')).toHaveCount(1, { timeout: 8000 });
    await sel.focus();                                     // a second open inside 30 s asks nothing
    await page.waitForTimeout(300);
    expect(posts).toBe(1);
  }
});

// The device's rate limit: a refresh within the hour answers "recent", and the page
// just shows the cached list's age - no polling, no "Checking".
test('model dropdown: a recent refresh shows the cached time without polling', async ({ page }) => {
  test.skip(ON_DEVICE, 'host mocks only');
  await seedToken(page);
  const s = cachedThenFresh();
  let gets = 0;
  await page.route('**/api/orch', (route) =>
    route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(orchPayload()) }));
  await page.route(/\/api\/models(\?.*)?$/, (route) => {
    gets++;
    return route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(s.freshBody) });
  });
  await page.route('**/api/models/refresh', (route) => route.fulfill({ status: 200, contentType: 'application/json',
    body: JSON.stringify({ ok: true, provider: 'anthropic', refresh: 'recent', fetchedAt: s.now }) }));
  await openApp(page);
  await page.locator('.tab[data-p=assistant]').click();
  await page.locator('.subtab[data-sp=llm]').click();
  const grp = page.locator('#subpane-llm details.setgroup').first();
  if (!(await grp.evaluate((d) => d.open))) await grp.locator('summary').click();
  const sel = page.locator('#orchM_anthropic');
  await expect(sel).toBeEnabled();
  const before = gets;
  await sel.focus();
  await expect(page.locator('#mupd_anthropic')).toHaveText(/^Updated \d/);
  await page.waitForTimeout(2500);
  expect(gets).toBe(before);   // nothing polled
});

// The poll never rebuilds a model select that did not change: once the per-model
// prices load, the build and the poll's sync must agree on the select's signature
// (they disagreed, so every 5 s poll replaced every model select).
test('model picker: the poll keeps an unchanged select once prices load', async ({ page }) => {
  test.skip(ON_DEVICE, 'host mocks only');
  test.setTimeout(30000);
  await seedToken(page);
  await page.route('**/api/orch', (route) =>
    route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(orchPayload()) }));
  await page.route(/\/api\/models(\?.*)?$/, (route) =>
    route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(modelsPayload()) }));
  await page.route('**/api/usage/models', (route) => route.fulfill({ status: 200, contentType: 'application/json',
    body: JSON.stringify({ asOf: '2026-10', models: [], residual: [], provRate: {},
      editable: { anthropic: [{ m: 'claude-opus-5', rIn: 5000000, rOut: 25000000, src: 'est', oIn: 0, oOut: 0 }] } }) }));
  await openApp(page);
  await page.locator('.tab[data-p=assistant]').click();
  await page.locator('.subtab[data-sp=llm]').click();
  const grp = page.locator('#subpane-llm details.setgroup').first();
  if (!(await grp.evaluate((d) => d.open))) await grp.locator('summary').click();
  const sel = page.locator('#orchM_anthropic');
  // Prices are in the labels once /api/usage/models answered.
  await expect(sel.locator('option', { hasText: 'per 1M' }).first()).toHaveCount(1, { timeout: 12000 });
  await sel.evaluate((s) => { s.__kept = 1; });
  await page.waitForTimeout(11000);   // two 5 s polls
  expect(await page.locator('#orchM_anthropic').evaluate((s) => s.__kept === 1)).toBe(true);
});
