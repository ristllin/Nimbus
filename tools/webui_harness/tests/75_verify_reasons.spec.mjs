// CUM (verify honesty): the provider verify badge renders a DISTINCT, honest line
// for each reason the verify surface (L1's /api/orch) reports, instead of a bare
// "key rejected" / "couldn't verify". Contract: each provider object may carry a
// `vfyReason` string on a non-verified row, one of:
//   nocredits | router_outdated | deferred | connectfail | tlsbusy
// A real pass (verify===1) always wins over any reason.
//
// T4 (host) tier: overrides /api/orch with a row per reason. Runs unchanged on a
// real device over LAN (T5/HIL). This spec is the DOM-dump evidence for DoD (e).
import { test, expect } from '@playwright/test';
import { seedToken, openApp } from './_helpers.mjs';

// One row per reason state so a single render shows every badge at once.
function orchPayload() {
  const prov = (hasKey, verify, vfyReason) => ({
    hasKey, verify, vts: verify === 1 ? 1700000000 : (vfyReason ? 1700000000 : 0),
    orchModel: '', subModel: '', choices: '', vfyReason,
  });
  return {
    running: true,
    providers: {
      cumulo: prov(true, 0, 'nocredits'),
      openai: prov(true, -1, 'router_outdated'),
      anthropic: prov(true, -1, 'connectfail'),
      mistral: prov(true, -1, 'tlsbusy'),
      zai: prov(true, -1, 'deferred'),
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

async function openModels(page, payload) {
  await seedToken(page);
  await page.route('**/api/orch', (route) => {
    if (route.request().method() === 'GET')
      return route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(payload) });
    return route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ ok: true }) });
  });
  await openApp(page);
  await page.locator('.tab[data-p=assistant]').click();
  await page.locator('.subtab[data-sp=llm]').click();
  // Force the "Providers & keys" group open. With nothing verified, applyOrch
  // auto-opens it on first load, so a toggle-click would close it again - set the
  // state directly instead of toggling.
  const grp = page.locator('#subpane-llm details.setgroup').first();
  await expect(page.locator('#provs .provrow').first()).toHaveCount(1);
  await grp.evaluate((el) => { el.open = true; });
  await expect(page.locator('#provs .provrow').first()).toBeVisible();
}

const EXPECT = {
  cumulo: 'Needs credits or subscription',
  openai: 'Router needs an update',
  anthropic: "Couldn't reach the provider",
  mistral: 'Busy now. Retry in a moment.',
  zai: 'Verification deferred',
};

test('each verify reason renders its own honest badge copy', async ({ page }, testInfo) => {
  await openModels(page, orchPayload());
  for (const [prov, txt] of Object.entries(EXPECT)) {
    await expect(page.locator(`#prov_${prov} .provhead .vfy`), `${prov} badge`).toHaveText(txt);
  }
  // No reason state falls back to a bare, undifferentiated "key rejected".
  await expect(page.locator('#provs')).not.toContainText('key rejected');
  await page.screenshot({ path: `screenshots/verify-reasons-${testInfo.project.name}.png`, fullPage: true });
});

test('a real pass overrides any stale reason', async ({ page }) => {
  const p = orchPayload();
  p.providers.cumulo = { hasKey: true, verify: 1, vts: 1700000000, orchModel: '', subModel: '', choices: '', vfyReason: 'nocredits' };
  await openModels(page, p);
  await expect(page.locator('#prov_cumulo .provhead .vfy')).toHaveText('verified');
});
