// QW5: a new user's first hour - the setup wizard and the pages it leads to.
//  - a key pasted under the wrong provider (the Cumulo default) is moved to its own,
//    and the page's key reader agrees with the device's (one shared table);
//  - every provider check shows its real outcome (rejected, no credit, unreachable,
//    a refused start), never a 40 s spin;
//  - the timezone step derives the device's POSIX zone from this browser;
//  - Notifier's consequences are stated, and confirmed, before choosing it;
//  - the Telegram step shows the token check's result;
//  - the place in the wizard is per device and never past an unmet required step;
//  - the summary's memory and ceiling lines say what is actually true.
// T4 (host) with a small stateful device stand-in; screenshots of every changed step
// land in screenshots/qw5-*.png for review.
import { test, expect } from '@playwright/test';
import { readFileSync } from 'node:fs';
import { seedToken } from './_helpers.mjs';
import { STATE, ORCH, MEM_STATS } from '../fixtures.mjs';

const ON_DEVICE = process.env.TARGET === 'device';
test.skip(ON_DEVICE, 'drives a scripted device stand-in (host tier only)');

const json = (route, body, status = 200) =>
  route.fulfill({ status, contentType: 'application/json', body: JSON.stringify(body) });

// A device stand-in: keys, verdicts and the verify task, enough for the wizard.
// verifyPlan[p] scripts the next check of p: {start:'ok'|409, why, land:{verify, vfyReason}, after:n}.
function makeDevice(opts = {}) {
  const dev = {
    state: { ...STATE, needsOnboarding: true, sta: true, provVerified: false, ...(opts.state || {}) },
    providers: {},
    posts: [],
    verifyPosts: [],
    pending: false,
    plan: opts.verifyPlan || {},
    tg: { verify: -1, vts: 0, bot: '' },
    mem: { ...MEM_STATS, embedAvailable: false, embed: { provider: 'openai', model: 'text-embedding-3-small', dims: 256 }, ...(opts.mem || {}) },
    ts: 1000,
  };
  for (const p of ['cumulo', 'openai', 'anthropic', 'mistral', 'zai'])
    dev.providers[p] = { hasKey: false, verify: -1, vts: 0, vfyReason: '', orchModel: '', subModel: '', choices: '', ...((opts.providers || {})[p] || {}) };
  dev.landIn = {};   // provider -> polls left before its verdict lands
  dev.land = (p) => {
    const plan = dev.plan[p] || { land: { verify: 1 } };
    const tgt = p === 'telegram' ? dev.tg : dev.providers[p];
    tgt.verify = plan.land.verify;
    tgt.vfyReason = plan.land.vfyReason || '';
    tgt.vts = ++dev.ts;
    if (p === 'telegram' && plan.land.verify === 1) tgt.bot = 'nimbus_test_bot';
    if (plan.land.verify === 1 && p !== 'telegram') dev.state.provVerified = true;
    dev.pending = false;
  };
  dev.tick = () => {
    for (const p of Object.keys(dev.landIn)) {
      if (--dev.landIn[p] <= 0) { delete dev.landIn[p]; dev.land(p); }
    }
  };
  return dev;
}

async function wire(page, dev) {
  await seedToken(page);
  await page.route('**/api/state', (r) => json(r, dev.state));
  await page.route('**/api/mem/stats', (r) => json(r, dev.mem));
  await page.route('**/api/config', (r) => {
    if (r.request().method() === 'POST') dev.posts.push(new URLSearchParams(r.request().postData() || ''));
    return json(r, { ok: true });
  });
  await page.route('**/api/orch', (r) => {
    const req = r.request();
    if (req.method() === 'POST') {
      const body = new URLSearchParams(req.postData() || '');
      dev.posts.push(body);
      const fields = { cumuloKey: 'cumulo', oaiKey: 'openai', antKey: 'anthropic', mistKey: 'mistral', zaiKey: 'zai' };
      for (const [f, p] of Object.entries(fields)) {
        if (!body.get(f)) continue;
        Object.assign(dev.providers[p], { hasKey: true, verify: -1, vts: 0, vfyReason: '' });
        // Cumulo and Z.ai saves start their own check on the device.
        if (p === 'cumulo' || p === 'zai') { dev.pending = true; dev.landIn[p] = (dev.plan[p] || {}).after || 2; }
      }
      if (body.get('tgToken')) { Object.assign(dev.tg, { verify: -1, vts: 0 }); dev.pending = true; dev.landIn.telegram = 2; }
      return json(r, { ok: true });
    }
    dev.tick();
    return json(r, { ...ORCH, providers: dev.providers, verifyPending: dev.pending, tgVerify: dev.tg.verify, tgVts: dev.tg.vts, tgBot: dev.tg.bot });
  });
  await page.route('**/api/verify', (r) => {
    const p = new URLSearchParams(r.request().postData() || '').get('provider');
    dev.verifyPosts.push(p);
    if (dev.pending) return json(r, { error: 'verify busy, retry', why: 'busy' }, 409);
    const plan = dev.plan[p] || {};
    if (plan.start === 409 && !plan.used) {
      plan.used = true;
      if (plan.why === 'turn') dev.landIn[p] = plan.after || 3;
      if (plan.why === 'low-memory') dev.land(p);
      return json(r, { error: 'verify busy, retry', why: plan.why }, 409);
    }
    dev.pending = true;
    dev.landIn[p] = plan.after || 2;
    return json(r, { ok: true });
  });
}

const shot = (page, name, info) =>
  page.screenshot({ path: `screenshots/qw5-${name}-${info.project.name}.png`, fullPage: true });

// ---- the key reader: page and device agree on every row ----------------------------
test('keyHome agrees with the device table (test/support/fixtures/key_home_cases.json)', async ({ page }) => {
  const tbl = JSON.parse(readFileSync(new URL('../../../test/support/fixtures/key_home_cases.json', import.meta.url)));
  await seedToken(page);
  await page.goto('/');
  for (const c of tbl.cases) {
    const key = c.prefix + c.fill.repeat(c.len) + c.suffix;
    expect(await page.evaluate((k) => keyHome(k), key), `key starting ${JSON.stringify(key.slice(0, 12))}`).toBe(c.home);
  }
});

// ---- the provider step -------------------------------------------------------------
test('an Anthropic key pasted under the Cumulo default is saved under Anthropic and verified', async ({ page }, info) => {
  const dev = makeDevice();
  await wire(page, dev);
  await page.goto('/?onboard=provider');
  await expect(page.locator('#onb_prov')).toHaveValue('cumulo');
  await expect(page.locator('#onb_key')).toHaveAttribute('placeholder', /cumulo_sk_/);
  await page.locator('#onb_key').fill('sk-ant-api03-' + 'g'.repeat(95));
  await page.locator('#onb_provsave').click();
  await expect(page.locator('#onb_provmsg')).toContainText('That is an Anthropic key, so it is saved under Anthropic.');
  await expect(page.locator('#onb_provmsg')).toContainText('Verified ✓');
  await expect(page.locator('#onb_prov')).toHaveValue('anthropic');
  const saved = dev.posts.filter((b) => b.has('antKey') || b.has('cumuloKey'));
  expect(saved.length).toBe(1);
  expect(saved[0].has('antKey')).toBe(true);
  await expect(page.locator('#onb_memmsg')).toContainText('Long-term memory needs a Cumulo Nimbus, OpenAI or Mistral key');
  await shot(page, 'provider-moved', info);
});

test('a key that is not a Cumulo key is refused under Cumulo, with where to go, and nothing is saved', async ({ page }) => {
  const dev = makeDevice();
  await wire(page, dev);
  await page.goto('/?onboard=provider');
  await page.locator('#onb_key').fill('not-a-known-shape-key-1234');
  await page.locator('#onb_provsave').click();
  await expect(page.locator('#onb_provmsg')).toContainText('That is not a Cumulo Nimbus key: those start with cumulo_sk_.');
  await page.locator('#onb_key').fill('123456789:' + 'B'.repeat(35));
  await page.locator('#onb_provsave').click();
  await expect(page.locator('#onb_provmsg')).toContainText('That is a Telegram bot token.');
  expect(dev.posts.length).toBe(0);
});

const OUTCOMES = [
  ['rejected', { cumulo: { land: { verify: 0 } } }, 'Cumulo Nimbus rejected this key. Check that you copied all of it, or create a new key at app.cumulo-nimbus.ai.'],
  ['nocredits', { cumulo: { land: { verify: -1, vfyReason: 'nocredits' } } }, 'The key works, but the Cumulo Nimbus account has no credit. Add credit at app.cumulo-nimbus.ai, then tap Save & Verify.'],
  ['unreachable', { cumulo: { land: { verify: -1, vfyReason: 'connectfail' } } }, "Couldn't reach Cumulo Nimbus. Check that the device's Wi-Fi has internet"],
  ['lowmemory', { cumulo: { land: { verify: -1, vfyReason: 'low-memory' } } }, 'Verification deferred: the device is low on working memory'],
  ['unexpected', { cumulo: { land: { verify: -1 } } }, "Couldn't verify: Cumulo Nimbus gave an unexpected answer."],
];
for (const [name, plan, text] of OUTCOMES) {
  test(`a Cumulo check that ends "${name}" says so in plain words`, async ({ page }, info) => {
    const dev = makeDevice({ verifyPlan: plan });
    await wire(page, dev);
    await page.goto('/?onboard=provider');
    await page.locator('#onb_key').fill('cumulo_sk_' + 'e'.repeat(48));
    await page.locator('#onb_provsave').click();
    await expect(page.locator('#onb_provmsg')).toContainText(text, { timeout: 15000 });
    await expect(page.locator('#onb_provmsg')).not.toContainText('Still verifying');
    // The device started the check itself on the save: the page did not ask again.
    expect(dev.verifyPosts).toEqual([]);
    if (name === 'rejected' || name === 'nocredits') await shot(page, `provider-${name}`, info);
  });
}

test('a refused start says why: a reply in flight, then the verdict lands', async ({ page }, info) => {
  const dev = makeDevice({ verifyPlan: { openai: { start: 409, why: 'turn', after: 3, land: { verify: 1 } } } });
  await wire(page, dev);
  await page.goto('/?onboard=provider');
  await page.locator('#onb_prov').selectOption('openai');
  await page.locator('#onb_key').fill('sk-proj-' + 'h'.repeat(48));
  await page.locator('#onb_provsave').click();
  await expect(page.locator('#onb_provmsg')).toContainText('The assistant is finishing a reply. Your key is saved and is checked right after.');
  await shot(page, 'provider-turn', info);
  await expect(page.locator('#onb_provmsg')).toContainText('Verified ✓', { timeout: 15000 });
  expect(dev.verifyPosts).toEqual(['openai']);
});

test('a provider verified before a reload counts, and a stored key can be re-checked without pasting', async ({ page }) => {
  const dev = makeDevice({ state: { provVerified: true }, providers: { mistral: { hasKey: true, verify: 1, vts: 5 } } });
  await wire(page, dev);
  await page.goto('/?onboard=provider');
  await expect(page.locator('#onb_provmsg')).toContainText('A provider is already verified.');
  await page.locator('#onbNext').click();
  await expect(page.locator('#onbTitle')).toHaveText('Your timezone');
  await page.locator('#onbBack').click();
  await page.locator('#onb_prov').selectOption('mistral');
  await page.locator('#onb_provsave').click();
  await expect(page.locator('#onb_provmsg')).toContainText('Verified ✓', { timeout: 15000 });
  expect(dev.verifyPosts).toEqual(['mistral']);
});

// ---- the timezone step ----------------------------------------------------------------
test.describe('timezone', () => {
  test.use({ timezoneId: 'Europe/London' });
  test('detects this browser\'s zone, saves its POSIX form, and confirms against the device clock', async ({ page }, info) => {
    const dev = makeDevice({ state: { provVerified: true } });
    const now = Math.floor(Date.now() / 1000);
    const d = new Date(now * 1000), pad = (n) => String(n).padStart(2, '0');
    const local = `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} ${pad(d.getHours())}:${pad(d.getMinutes())}`;
    dev.state.clock = { synced: true, epoch: now, local, tz: '' };
    await wire(page, dev);
    await page.goto('/?onboard=timezone');
    await expect(page.locator('#onb_tzdetect')).toContainText('Europe/London');
    await expect(page.locator('#onb_tzdetect')).toContainText('GMT0BST,M3.5.0/1,M10.5.0');
    await page.locator('#onb_tzuse').click();
    await expect(page.locator('#onb_tzmsg')).toContainText('Saved: Europe/London. Device time is', { timeout: 8000 });
    await expect(page.locator('#onb_tzmsg')).toContainText('the same as this computer');
    const tzPost = dev.posts.find((b) => b.has('devTz'));
    expect(tzPost.get('devTz')).toBe('GMT0BST,M3.5.0/1,M10.5.0');
    await shot(page, 'timezone', info);
  });
});

// Zone -> the POSIX rule the page derives (2026). Every one was checked hour by hour
// against the IANA zone for 2026-2028 (lanes/QW5/evidence/tz-semantic-check.txt).
const ZONES = {
  'UTC': 'UTC0',
  'Europe/London': 'GMT0BST,M3.5.0/1,M10.5.0',
  'Europe/Paris': 'CET-1CEST,M3.5.0,M10.5.0/3',
  'America/New_York': 'EST5EDT,M3.2.0,M11.1.0',
  'America/Los_Angeles': 'PST8PDT,M3.2.0,M11.1.0',
  'America/Phoenix': 'MST7',
  'America/St_Johns': '<-0330>3:30<-0230>,M3.2.0,M11.1.0',
  'Asia/Kolkata': '<+0530>-5:30',
  'Asia/Tokyo': '<+09>-9',
  'Asia/Jerusalem': 'IST-2IDT,M3.4.4/26,M10.5.0',
  'Australia/Sydney': '<+10>-10<+11>,M10.1.0,M4.1.0/3',
  'Pacific/Chatham': '<+1245>-12:45<+1345>,M9.5.0/2:45,M4.1.0/3:45',
};
for (const [zone, posix] of Object.entries(ZONES)) {
  test.describe(`zone ${zone}`, () => {
    test.use({ timezoneId: zone });
    test(`derives ${posix}`, async ({ page }) => {
      await seedToken(page);
      await page.goto('/');
      const got = await page.evaluate(() => tzFromBrowser(new Date(Date.UTC(2026, 5, 1))));
      expect(got.iana).toBeTruthy();   // Chromium may name a legacy alias (Asia/Calcutta)
      expect(got.posix).toBe(posix);
      expect(got.posix.length).toBeLessThanOrEqual(48);   // the device's devTz bound
    });
  });
}

// ---- mode, Telegram -------------------------------------------------------------------
test('the mode step says what Notifier turns off and confirms before choosing it', async ({ page }, info) => {
  const dev = makeDevice({ state: { provVerified: true, mode: 1 } });
  await wire(page, dev);
  await page.goto('/?onboard=mode');
  const body = page.locator('#onbBody');
  await expect(body).toContainText('Notifier turns Wi-Fi and this web page off');
  await expect(body).toContainText('Settings > Mode on the device');
  await expect(body).not.toContainText('anytime from the header');
  await shot(page, 'mode', info);
  await page.locator('.onbmb[data-m="0"]').click();
  await expect(page.locator('#modalOv.show')).toBeVisible();
  await expect(page.locator('#modalOv')).toContainText('Wi-Fi and this web page turn off');
  await page.locator('#modalCancel').click();
  await expect(page.locator('.onbmb[data-m="1"]')).toHaveClass(/on/);
  await page.locator('.onbmb[data-m="0"]').click();
  await page.locator('#modalOk').click();
  await expect(page.locator('.onbmb[data-m="0"]')).toHaveClass(/on/);
});

test('the Telegram step shows the token check, with the bot it reached', async ({ page }, info) => {
  const dev = makeDevice({ state: { provVerified: true } });
  await wire(page, dev);
  await page.goto('/?onboard=telegram');
  await page.locator('#onb_tg').fill('123456789:' + 'B'.repeat(35));
  await page.locator('#onb_tgsave').click();
  await expect(page.locator('#onb_tgmsg')).toContainText('Verified ✓ Connected to @nimbus_test_bot.', { timeout: 15000 });
  await expect(page.locator('#onbBody')).toContainText('no restart needed');
  await shot(page, 'telegram', info);
});

test('a rejected Telegram token says so, with the fix', async ({ page }) => {
  const dev = makeDevice({ state: { provVerified: true }, verifyPlan: { telegram: { land: { verify: 0 } } } });
  await wire(page, dev);
  await page.goto('/?onboard=telegram');
  await page.locator('#onb_tg').fill('123456789:' + 'B'.repeat(35));
  await page.locator('#onb_tgsave').click();
  await expect(page.locator('#onb_tgmsg')).toContainText('Telegram rejected this token. Copy it again from @BotFather', { timeout: 15000 });
});

// ---- resume per device ----------------------------------------------------------------
test('the saved step is per device and never past an unmet required step', async ({ page }) => {
  const dev = makeDevice({ state: { sta: true, staIp: '192.168.1.42', provVerified: true } });
  await wire(page, dev);
  await page.goto('/?onboard=telegram');
  await expect(page.locator('#onbTitle')).toHaveText('Telegram');
  // Reload: same device, same step.
  await page.goto('/');
  await expect(page.locator('#onbTitle')).toHaveText('Telegram');
  // The legacy per-origin key is ignored, and the same device after a factory reset
  // (no Wi-Fi, nothing verified) never resumes mid-wizard.
  dev.state = { ...dev.state, sta: false, staIp: '', provVerified: false };
  await page.evaluate(() => localStorage.setItem('nimbusOnboardStep', '6'));
  await page.goto('/');
  await expect(page.locator('#onbTitle')).toHaveText('Connect to Wi-Fi');
  // Another device (another sign-in code) at the same address starts at Welcome, even
  // with the first device's step saved in this browser.
  dev.state = { ...dev.state, sta: true, staIp: '192.168.1.42', provVerified: true };
  await page.goto('/?onboard=telegram');
  await expect(page.locator('#onbTitle')).toHaveText('Telegram');
  const other = await page.context().newPage();
  await wire(other, dev);
  await seedToken(other, 'ANOTHERDEVICE0001');
  await other.goto('/');
  await expect(other.locator('#onbTitle')).toHaveText('Welcome');
});

test('the welcome step says to have the key ready and gives one time estimate', async ({ page }, info) => {
  const dev = makeDevice({ state: { sta: false, provVerified: false } });
  await wire(page, dev);
  await page.goto('/');
  await expect(page.locator('#onbBody')).toContainText('about five minutes');
  await expect(page.locator('#onbBody')).toContainText('Have your AI provider key ready');
  await expect(page.locator('#onbBody')).not.toContainText('about a minute');
  await shot(page, 'welcome', info);
});

// ---- the summary --------------------------------------------------------------------
test('the summary says the ceiling is not counting yet while the clock has not synced', async ({ page }, info) => {
  const dev = makeDevice({ state: { provVerified: true, spend: { ...STATE.spend, loaded: false }, clock: { ...STATE.clock, tz: '' } },
    mem: { embedAvailable: true, embed: { provider: 'cumulo', model: 'text-embedding-3-small', dims: 256 } } });
  await wire(page, dev);
  await page.goto('/?onboard=done');
  const sum = page.locator('#onb_summary');
  await expect(sum).toContainText('Monthly spend ceiling: $20.00 across all providers. It starts counting once the device clock has synced');
  await expect(sum).toContainText('Long-term memory: on, through Cumulo Nimbus.');
  await expect(sum).toContainText('Timezone: UTC (not set');
  await shot(page, 'done', info);
});

test('the summary says the ceiling is off when provider limits replace it, and memory is off without a key', async ({ page }) => {
  const dev = makeDevice({ state: { provVerified: true, spend: { ...STATE.spend, active: false, providers: [{ p: 'anthropic', capCents: 500 }] } } });
  await wire(page, dev);
  await page.goto('/?onboard=done');
  const sum = page.locator('#onb_summary');
  await expect(sum).toContainText('Monthly spend ceiling: off. Your per-provider limits apply instead');
  await expect(sum).toContainText('Long-term memory needs a Cumulo Nimbus, OpenAI or Mistral key');
});

test('the provider step does not promise a ceiling that is not in force', async ({ page }) => {
  const dev = makeDevice({ state: { spend: { ...STATE.spend, loaded: false } } });
  await wire(page, dev);
  await page.goto('/?onboard=provider');
  const d = page.locator('#onb_defaults');
  await expect(d).toContainText('standard model for the assistant');
  await expect(d).not.toContainText('Paid AI calls pause once');
  await expect(d).toContainText('It starts counting once the device clock has synced');
});

// ---- the Memory page and the Software update panel ------------------------------------
test('the Memory page says plainly that memory needs an embeddings key', async ({ page }, info) => {
  const dev = makeDevice({ state: { needsOnboarding: false } });
  await wire(page, dev);
  await page.goto('/');
  await page.locator('.tab[data-p=memory]').click();
  const grp = page.locator('details.setgroup:has(#memstat)');
  await grp.evaluate((el) => { el.open = true; });
  await expect(page.locator('#embstate')).toContainText('Long-term memory is off: it needs a Cumulo Nimbus, OpenAI or Mistral key');
  await expect(page.locator('#memstat')).not.toContainText('no key');
  await shot(page, 'memory-off', info);
});

test('the Software update panel shows the release notes line', async ({ page }) => {
  const dev = makeDevice({ state: { needsOnboarding: false, fw: 'v5.0.0', otaLatest: 'v5.0.0', ota: 'up-to-date', otaResult: 'up-to-date',
    otaNotes: 'First public release: guided setup, honest errors, a monthly spend ceiling.' } });
  await wire(page, dev);
  await page.goto('/');
  await page.locator('.tab[data-p=device]').click();
  await page.locator('#fwsec').evaluate((el) => { el.open = true; });
  await expect(page.locator('#fwNotes')).toHaveText('In this version: First public release: guided setup, honest errors, a monthly spend ceiling.');
  await expect(page.locator('#fwLatest')).toHaveText('v5.0.0');
});
