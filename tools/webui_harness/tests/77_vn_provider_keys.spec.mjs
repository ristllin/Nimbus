// CUM-445: on a Virtual Nimbus, a direct provider key pasted in the web UI must reach
// /api/orch under the SAME field name nimbusd accepts. The bug: nimbusd read
// openaiKey/mistralKey/anthropicKey while the shared web app posts oaiKey/antKey/mistKey
// (the canonical provider_slots.h keyField), so every direct-provider write fell through
// to a silent {ok:true} and nothing applied. This spec proves the UI side of the fix -
// the exact field the page posts - against the HOSTED (nimbusd) page. The daemon side
// (that these field names are accepted, and an unknown one is refused) is proven by the
// nimbusd host suite testEveryFieldNameApplies / testUnknownKeyFieldRefused.
//
// The served page is the exact assembled web app nimbusd serves (byte-parity with
// webui_page.h), so the field the UI posts here is the field the VN posts. Reads only
// the captured POST body + visible DOM, so it also runs against a real nimbusd-served
// page over LAN (TARGET=device) unchanged.
import { test, expect } from '@playwright/test';
import { seedToken, openApp } from './_helpers.mjs';

// A keyless /api/orch payload with every first-class provider present, so each row
// renders with an empty key input to paste into. Shape mirrors buildOrchState.
function orchPayload() {
  const prov = (choices) => ({ hasKey: false, verify: -1, vts: 0, orchModel: '', subModel: '', choices });
  return {
    running: true,
    providers: {
      cumulo: prov(''),
      openai: prov('gpt-5.6,gpt-5.6-luna'),
      anthropic: prov('claude-opus-5,claude-sonnet-5'),
      mistral: prov('mistral-large-latest'),
      zai: prov(''),
    },
    cust: { base: '', conv: '', model: '', hasKey: false },
    orchHost: '', provPrio: 'mistral,openai,anthropic', subPrio: 'mistral,openai,anthropic',
    sttProv: 'openai', ttsOn: false, ttsProv: 'openai', ttsVoice: '',
    theme: 'nimbus', hasTav: false, hasTg: false,
    tgAllow: '', tgBot: '', tgLive: false, tgVerify: -1, tgVts: 0, tavVerify: -1, tavVts: 0,
    fetchPol: 'ask', compactKB: 0, loopRounds: 0, loopDeadline: '', orchLoop: false,
    orchTrace: false, midFail: 0, tlsSlots: 1, tlsVerify: 1,
    modInbound: 0, modOutbound: 0, modInjection: 0,
    sfxLvlN: 2, sfxLvlO: 2, sfxTheme: 'terran', sfxVol: 60, sfxTier: 'basic', sfxSync: 'idle',
    usage: { sessIn: 0, sessOut: 0, turns: 0, lastIn: 0, lastOut: 0, byProvider: [] },
    jobs: [],
  };
}

// GET serves the payload; POST is captured (so a test can assert the posted field).
async function routeOrch(page, onPost) {
  await page.route('**/api/orch', (route) => {
    const req = route.request();
    if (req.method() === 'GET')
      return route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(orchPayload()) });
    if (onPost) onPost(req.postData());
    return route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ ok: true, applied: 1 }) });
  });
  await page.route('**/api/verify', (route) =>
    route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ ok: true }) }));
}

async function openModels(page) {
  await seedToken(page);
  await openApp(page);
  await page.locator('.tab[data-p=assistant]').click();
  await page.locator('.subtab[data-sp=llm]').click();
  // "Providers & keys" is the first collapsed <details> group; open it to reveal the
  // rows. Set open directly (not a summary click) so an applyOrch re-render that races
  // the toggle cannot leave it collapsed.
  await page.locator('#subpane-llm details.setgroup').first().evaluate((d) => { d.open = true; });
  await expect(page.locator('#provs .provrow').first()).toBeVisible();
}

// Each direct BYOK provider key must post under the field nimbusd now accepts, and
// NEVER under the old names the daemon used to read (which it now refuses with 400).
const CASES = [
  { slug: 'openai',    field: 'oaiKey',  key: 'sk_openai_TESTKEY_abcd',  old: 'openaiKey' },
  { slug: 'anthropic', field: 'antKey',  key: 'sk-ant-TESTKEY-abcd',     old: 'anthropicKey' },
  { slug: 'mistral',   field: 'mistKey', key: 'mistral_TESTKEY_abcd',    old: 'mistralKey' },
];

for (const cse of CASES) {
  test(`VN: a ${cse.slug} key posts ${cse.field} (the field nimbusd accepts), not ${cse.old} or mistKey`, async ({ page }) => {
    let posted = null;
    await routeOrch(page, (body) => { posted = body; });
    await openModels(page);
    await page.locator(`#key_${cse.slug}`).fill(cse.key);
    await page.locator(`#vfy_${cse.slug}`).click();
    await expect.poll(() => posted).toContain(`${cse.field}=`);
    // Never the pre-CUM-445 daemon field name, and never another provider's slot.
    expect(posted).not.toContain(`${cse.old}=`);
    if (cse.field !== 'mistKey') expect(posted).not.toContain('mistKey=');
  });
}
