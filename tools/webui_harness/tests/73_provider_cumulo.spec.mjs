// CUM-201: the device Models UI must expose a first-class Cumulo Nimbus provider
// slot, rendered FIRST (the recommended one-key path), plus Z.ai; each provider's
// key must route to the right /api/orch field (cumulo -> cumuloKey, not the old
// silent mistKey fallback); and a cumulo_sk_ key pasted into a direct provider
// must be pointed at the Cumulo slot instead of a bare "key rejected".
//
// T4 (host) tier: overrides /api/orch with a complete payload that mirrors
// buildOrchState (providers keyed by name in render order, cumulo first). Runs
// unchanged on a real device over LAN (T5/HIL) - only the mock overrides differ.
import { test, expect } from '@playwright/test';
import { seedToken, openApp } from './_helpers.mjs';

// A complete /api/orch payload: each provider carries the full shape applyOrch
// consumes (numeric verify, vts, orchModel/subModel, choices) so the rows render.
function orchPayload() {
  const prov = (hasKey, verify, choices) => ({
    hasKey, verify, vts: verify === -1 ? 0 : 1700000000,
    orchModel: '', subModel: '', choices,
  });
  return {
    running: true,
    providers: {
      cumulo: prov(false, -1, ''),
      openai: prov(false, -1, 'gpt-5.5,gpt-5.4-mini'),
      anthropic: prov(true, 1, 'claude-opus-4-8,claude-sonnet-4-6'),
      mistral: prov(false, -1, 'mistral-large-latest'),
      zai: prov(false, -1, ''),
    },
    cust: { base: '', conv: '', model: '', hasKey: false },
    orchHost: 'cumulo',
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

// Route /api/orch: GET serves the payload; POST is captured and answered ok.
// Returns a getter for the last POST body so a test can assert key routing.
async function routeOrch(page, payload, onPost) {
  await page.route('**/api/orch', (route) => {
    const req = route.request();
    if (req.method() === 'GET')
      return route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(payload) });
    if (onPost) onPost(req.postData());
    return route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ ok: true }) });
  });
  await page.route('**/api/verify', (route) =>
    route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ ok: true }) }));
}

async function openModels(page) {
  await seedToken(page);
  await openApp(page);
  await page.locator('.tab[data-p=assistant]').click();
  await page.locator('.subtab[data-sp=llm]').click();
  // "Providers & keys" is a collapsed <details> group; open it to reveal the rows.
  await page.locator('#subpane-llm details.setgroup').first().locator('summary').click();
  await expect(page.locator('#provs .provrow').first()).toBeVisible();
}

test('Cumulo Nimbus is the first provider row, marked recommended, with Z.ai present', async ({ page }, testInfo) => {
  await routeOrch(page, orchPayload());
  await openModels(page);
  const first = page.locator('#provs .provrow').first();
  await expect(first.locator('.provhead b')).toHaveText('Cumulo Nimbus');
  await expect(first).toContainText('Recommended');
  await expect(first).toContainText('no URL to type'); // carries the router wiring, no URL (item 2)
  await expect(page.locator('#key_cumulo')).toHaveAttribute('placeholder', /cumulo_sk_/);
  await expect(page.locator('#prov_zai')).toBeVisible(); // Z.ai surfaced too (item 4)
  await expect(page.locator('#prov_zai .provhead b')).toHaveText('Z.ai');
  // Primary provider select can now choose Cumulo Nimbus + Z.ai.
  const opts = await page.locator('#orchHost option').allTextContents();
  expect(opts).toContain('Cumulo Nimbus');
  expect(opts).toContain('Z.ai');
  // Per-size visual evidence (desktop + phone projects).
  await page.screenshot({ path: `screenshots/cum201-models-${testInfo.project.name}.png`, fullPage: true });
});

test('a Cumulo key routes to cumuloKey, never the Mistral slot', async ({ page }) => {
  let posted = null;
  await routeOrch(page, orchPayload(), (body) => { posted = body; });
  await openModels(page);
  await page.locator('#key_cumulo').fill('cumulo_sk_testkey_abcd');
  await page.locator('#vfy_cumulo').click();
  await expect.poll(() => posted).toContain('cumuloKey=');
  expect(posted).not.toContain('mistKey=');
});

test('a Z.ai key routes to zaiKey', async ({ page }) => {
  let posted = null;
  await routeOrch(page, orchPayload(), (body) => { posted = body; });
  await openModels(page);
  await page.locator('#key_zai').fill('zai_test_key');
  await page.locator('#vfy_zai').click();
  await expect.poll(() => posted).toContain('zaiKey=');
});

test('a cumulo_sk_ key pasted into OpenAI is pointed at the Cumulo slot, not saved', async ({ page }) => {
  let posted = false;
  await routeOrch(page, orchPayload(), () => { posted = true; });
  await openModels(page);
  await page.locator('#key_openai').fill('cumulo_sk_wrongslot_xyz');
  await page.locator('#vfy_openai').click();
  const msg = page.locator('#pmsg_openai');
  await expect(msg).toContainText('Cumulo Nimbus');
  await expect(msg).toContainText('above');
  // The misplaced key was NOT written to any provider slot (no POST fired).
  await page.waitForTimeout(300);
  expect(posted).toBe(false);
});
