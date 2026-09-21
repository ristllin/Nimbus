// CUM-447: a low-memory verify deferral must EXPLAIN itself. The owner pasted a key
// on Lumi and saw a bare "Verification deferred" pill with no cause and no next step.
// The device now reports vfyReason:"low-memory" plus the measured largest free block
// (vfyMax8) and the gate (vfyFloor); the web app turns that into:
//   - the pill's hover (title): the full "what happened / what happens next" line
//     plus the measured number ("Largest free block 11 KB, needs 8 KB"),
//   - a one-line hint under the key field carrying the same sentence,
//   - the Verify button still enabled so the owner can force a retry.
//
// T4 (host) tier: overrides /api/orch with a deferred row. Runs unchanged on a real
// device over LAN (T5/HIL). DOM-dump + screenshot evidence for the DoD.
import { test, expect } from '@playwright/test';
import { seedToken, openApp } from './_helpers.mjs';

const FULL_MSG =
  'Verification deferred: the device is low on working memory right now. Your key is saved; it is checked again automatically when memory frees up, or tap Verify to retry.';

function orchPayload() {
  const base = (extra) => ({
    hasKey: true, orchModel: '', subModel: '', choices: '', vfyReason: '', vts: 0, ...extra,
  });
  return {
    running: true,
    providers: {
      // 11 KB largest free block, 8 KB gate -> deferred low-memory.
      mistral: base({ verify: -1, vfyReason: 'low-memory', vts: 1700000000, vfyMax8: 11264, vfyFloor: 8000 }),
      // A verified provider must carry NO deferred hint.
      openai: base({ verify: 1, vts: 1700000000, vfyMax8: 0, vfyFloor: 8000 }),
    },
    cust: { base: '', conv: 'openai', model: '', hasKey: false },
    orchHost: 'mistral',
    provPrio: 'mistral', subPrio: 'mistral',
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
  const grp = page.locator('#subpane-llm details.setgroup').first();
  await expect(page.locator('#provs .provrow').first()).toHaveCount(1);
  await grp.evaluate((el) => { el.open = true; });
  await expect(page.locator('#provs .provrow').first()).toBeVisible();
}

test('a low-memory deferral explains the cause, the number, and the next step', async ({ page }, testInfo) => {
  await openModels(page, orchPayload());

  // The pill still reads the short, honest label.
  const pill = page.locator('#prov_mistral .provhead .vfy');
  await expect(pill).toHaveText('Verification deferred');

  // Its hover carries the full sentence AND the measured number.
  const title = await pill.getAttribute('title');
  expect(title).toContain(FULL_MSG);
  expect(title).toContain('Largest free block 11 KB, needs 8 KB');

  // The one-line hint under the key field carries the same sentence.
  const hint = page.locator('#dhint_mistral');
  await expect(hint).toHaveText(FULL_MSG);

  // The Verify button is present and enabled so the owner can force a retry now.
  const vfy = page.locator('#vfy_mistral');
  await expect(vfy).toHaveText('Verify');
  await expect(vfy).toBeEnabled();

  await page.screenshot({ path: `screenshots/verify-deferred-lowmem-${testInfo.project.name}.png`, fullPage: true });
});

test('a verified provider shows no deferred hint', async ({ page }) => {
  await openModels(page, orchPayload());
  await expect(page.locator('#prov_openai .provhead .vfy')).toHaveText('verified');
  await expect(page.locator('#dhint_openai')).toHaveText('');
});
