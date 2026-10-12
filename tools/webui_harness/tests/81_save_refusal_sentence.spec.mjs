// CUM-514: a value the device refuses answers 400 with the sentence to show (the
// key-shape rule, a connector token with a space in it, a directive that is too
// long). Every save helper used to collapse a 400 into "Couldn't save - try again",
// so that sentence never reached the owner, and retrying never cleared it. jok()
// now keeps a 400 body's sentence the way it keeps a 507's, for orchApply (Models
// Save & verify, onboarding), the connector saves and every other caller. A 400
// that carries a code ("bad_action") is not a sentence and keeps the fallback.
// The refusals are mocked with page.route, so this is a host-harness spec (T4); on
// a real device the same sentence comes from POST /api/orch itself.
import { test, expect } from '@playwright/test';
import { seedToken, openApp } from './_helpers.mjs';

const KEY_SENTENCE = 'That Mistral key has a space or a hidden character in it';
const TOKEN_SENTENCE =
  'The token for github has a space or a hidden character in it. Paste just the token and save again.';

// A keyless /api/orch payload with every first-class provider present (the shape
// buildOrchState serves), so each provider row renders an empty key input.
function orchPayload() {
  const prov = (choices) => ({ hasKey: false, verify: -1, vts: 0, orchModel: '', subModel: '', choices });
  return {
    running: true,
    providers: {
      cumulo: prov(''),
      openai: prov('gpt-5.6'),
      anthropic: prov('claude-sonnet-5'),
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

// GET serves the payload; every POST is refused with `refusal` as the 400 body.
async function routeOrchRefusing(page, refusal) {
  await page.route('**/api/orch', (route) => {
    if (route.request().method() === 'GET')
      return route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(orchPayload()) });
    return route.fulfill({ status: 400, contentType: 'application/json', body: JSON.stringify(refusal) });
  });
  await page.route('**/api/verify', (route) =>
    route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ ok: true }) }));
}

async function openModels(page) {
  await seedToken(page);
  await openApp(page);
  await page.locator('.tab[data-p=assistant]').click();
  await page.locator('.subtab[data-sp=llm]').click();
  await page.locator('#subpane-llm details.setgroup').first().evaluate((d) => { d.open = true; });
  await expect(page.locator('#provs .provrow').first()).toBeVisible();
}

test('Models: a refused key shows the device sentence, not "Couldn\'t save"', async ({ page }) => {
  await routeOrchRefusing(page, { error: KEY_SENTENCE });
  await openModels(page);
  await page.locator('#key_mistral').fill('mistral_TESTKEY_abcd ');
  await page.locator('#vfy_mistral').click();
  await expect(page.locator('#pmsg_mistral')).toContainText(KEY_SENTENCE);
  await expect(page.locator('#pmsg_mistral')).not.toContainText("Couldn't save");
  await expect(page.locator('#toast')).toContainText(KEY_SENTENCE);
});

test('Models: a 400 that carries a code, not a sentence, keeps the plain fallback', async ({ page }) => {
  await routeOrchRefusing(page, { error: 'bad_action' });
  await openModels(page);
  await page.locator('#key_mistral').fill('mistral_TESTKEY_abcd');
  await page.locator('#vfy_mistral').click();
  await expect(page.locator('#pmsg_mistral')).toContainText("Couldn't save - try again.");
  await expect(page.locator('#pmsg_mistral')).not.toContainText('bad_action');
});

test('Connectors: a refused token shows the device sentence', async ({ page }) => {
  const known = [
    { id: 'github', name: 'GitHub', providers: 'openai,anthropic,mistral', kind: 'mcp', cid: '', cred: 'GitHub PAT', desc: 'Repos.', docs: '' },
  ];
  const body = { configured: [], known, keyed: { anthropic: true }, host: 'anthropic', wsChecking: false };
  await page.route('**/api/connectors**', (route) => {
    if (route.request().method() === 'GET')
      return route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(body) });
    return route.fulfill({ status: 400, contentType: 'application/json', body: JSON.stringify({ error: TOKEN_SENTENCE }) });
  });
  await seedToken(page);
  await openApp(page);
  await page.locator('.tab[data-p=assistant]').click();
  await page.locator('.subtab[data-sp=connectors]').click();
  await expect(page.locator('#cc_github_tok')).toBeAttached();
  await page.locator('#cc_github_prov').selectOption('anthropic');
  await page.locator('#cc_github_tok').fill('ghp_abc def');
  await page.locator(`button[onclick="saveConnCard('github')"]`).click();
  await expect(page.locator('#toast')).toContainText(TOKEN_SENTENCE);
});
