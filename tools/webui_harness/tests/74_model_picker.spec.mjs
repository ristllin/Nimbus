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
