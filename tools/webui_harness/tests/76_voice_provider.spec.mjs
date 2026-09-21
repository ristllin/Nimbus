// CUM-439: Cumulo Nimbus must be a selectable VOICE provider (STT + TTS) in the
// web Voice settings, alongside Mistral and OpenAI. The routing core (CUM-376)
// already resolves cumulo; this suite pins the CHOOSE-it surface:
//   1. Both the Dictation (#sttProv) and Spoken replies (#ttsProv) selects offer
//      a "Cumulo Nimbus" option (value=cumulo).
//   2. Picking Cumulo Nimbus persists it (POST /api/orch sttProv/ttsProv=cumulo).
//   3. The voice picker shows an HONEST no-key hint when no Cumulo key is set,
//      and the router's voice defaults (the OpenAI upstream set) once it is.
//
// T4 (host) tier: overrides /api/orch (mirrors buildOrchState) and /api/voices.
// Runs unchanged on a real device over LAN (T5/HIL) - only the mock differs.
import { test, expect } from '@playwright/test';
import { seedToken, openApp } from './_helpers.mjs';

// A complete /api/orch payload. `cumuloKey` toggles whether the device holds a
// Cumulo key; `voiceProv` sets both voice surfaces to that provider.
function orchPayload({ cumuloKey = false, voiceProv = 'mistral' } = {}) {
  const prov = (hasKey, verify, choices) => ({
    hasKey, verify, vts: verify === -1 ? 0 : 1700000000,
    orchModel: '', subModel: '', choices,
  });
  return {
    running: true,
    providers: {
      cumulo: prov(cumuloKey, cumuloKey ? 1 : -1, ''),
      openai: prov(false, -1, 'gpt-5.5,gpt-5.4-mini'),
      anthropic: prov(true, 1, 'claude-opus-4-8'),
      mistral: prov(true, 1, 'mistral-large-latest'),
      zai: prov(false, -1, ''),
    },
    cust: { base: '', conv: '', model: '', hasKey: false },
    orchHost: 'mistral',
    provPrio: 'mistral,anthropic', subPrio: 'mistral,anthropic',
    sttProv: voiceProv, ttsOn: true, ttsProv: voiceProv, ttsVoice: '',
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

// Route /api/orch (GET serves payload, POST captured) and /api/voices (empty, so
// the client falls back to its bundled catalog - deterministic, no mock coupling).
async function routeOrch(page, payload, onPost) {
  await page.route('**/api/orch', (route) => {
    const req = route.request();
    if (req.method() === 'GET')
      return route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(payload) });
    if (onPost) onPost(req.postData());
    return route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ ok: true }) });
  });
  await page.route('**/api/voices**', (route) =>
    route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify([]) }));
}

// Assistant tab -> Models subtab -> open the Voice group.
async function openVoice(page) {
  await seedToken(page);
  await openApp(page);
  await page.locator('.tab[data-p=assistant]').click();
  await page.locator('.subtab[data-sp=llm]').click();
  await page.locator('#subpane-llm details.setgroup summary', { hasText: 'Voice' }).click();
  await expect(page.locator('#sttProv')).toBeVisible();
}

test('both voice selects offer a Cumulo Nimbus option', async ({ page }, testInfo) => {
  await routeOrch(page, orchPayload());
  await openVoice(page);
  for (const id of ['#sttProv', '#ttsProv']) {
    const opt = page.locator(`${id} option[value=cumulo]`);
    await expect(opt).toHaveCount(1);
    await expect(opt).toHaveText('Cumulo Nimbus');
  }
  await page.screenshot({ path: `screenshots/cum439-voice-picker-${testInfo.project.name}.png`, fullPage: true });
});

test('picking Cumulo Nimbus persists to both voice surfaces', async ({ page }) => {
  const posts = [];
  await routeOrch(page, orchPayload({ cumuloKey: true }), (b) => posts.push(b));
  await openVoice(page);
  await page.locator('#sttProv').selectOption('cumulo');
  await expect.poll(() => posts.some((b) => b && b.includes('sttProv=cumulo'))).toBe(true);
  await page.locator('#ttsProv').selectOption('cumulo');
  await expect.poll(() => posts.some((b) => b && b.includes('ttsProv=cumulo'))).toBe(true);
});

test('honest no-key hint when Cumulo Nimbus voice is chosen without a Cumulo key', async ({ page }) => {
  await routeOrch(page, orchPayload({ cumuloKey: false, voiceProv: 'cumulo' }));
  await openVoice(page);
  await expect(page.locator('#voiceHint')).toHaveText('Needs a Cumulo key to use this.');
});

test('with a Cumulo key set, the voice picker shows the router voice defaults, not the no-key hint', async ({ page }) => {
  await routeOrch(page, orchPayload({ cumuloKey: true, voiceProv: 'cumulo' }));
  await openVoice(page);
  // Hint reads as ready (not the needs-a-key line), and the default voice "alloy"
  // (the core voiceRouteFor cumulo TTS default) is offered.
  await expect(page.locator('#voiceHint')).toContainText('Cumulo Nimbus voices');
  await expect(page.locator('#voiceHint')).not.toContainText('Needs a Cumulo key');
  await expect(page.locator('#ttsVoice option[value=alloy]')).toHaveCount(1);
});
