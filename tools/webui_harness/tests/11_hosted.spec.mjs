// Hosted (Virtual Nimbus) honest-UI + pane-switch robustness (CUM-279).
//
// The live hosted page hit two things a device-only harness never did:
//   1. Device-only chrome shown faked or dead on a hosted instance (a "Free RAM
//      65536K of 65536K" tile, an "ap undefined - undefined" network line, dead
//      audio-test buttons, an ESP OTA card that a VN cannot use).
//   2. A tab click bouncing back to Home / navigation dying - a hosted-only state
//      path threw during a pane loader and took the whole nav down. The sparse-
//      payload robustness suite did NOT set the hosted flag, so it missed the live
//      trigger. These specs set window.NIMBUS_HOSTED (as nimbusd's bootstrap does).
//
// Every check reads console + pageerror or the visible DOM, so it also runs against
// a real nimbusd-served page over LAN (TARGET=device) unchanged.
import { test, expect } from '@playwright/test';
import { seedToken, openApp, collectErrors } from './_helpers.mjs';

// Mark the page hosted before any app script runs (nimbusd injects this in its page
// bootstrap). Must be called before openApp().
async function markHosted(page) {
  await page.addInitScript(() => { window.NIMBUS_HOSTED = true; });
}

// A realistic hosted /api/state: a real container heap (not the device cap), no AP /
// station / mDNS, Orchestrator mode, no ota* fields, no PSRAM, external power.
const HOSTED_STATE = {
  virtual: true, host: 'nimbusd', fw: 'v0', mode: 1, jobs: 0, running: true,
  needsOnboarding: false, provVerified: false, hasRing: false,
  heap: 44 * 1024 * 1024, heapTotal: 64 * 1024 * 1024,
  storeSD: true, storeLabel: 'instance disk', storeTotal: 0, storeFree: 0,
  batt: { valid: false, onExtPower: true },
  clock: { synced: true, epoch: Date.now() / 1000, local: '2026-09-01 00:00', tz: 'UTC' },
  profile: 1, effectiveProfile: 1, effectiveProfileName: 'Balanced',
};

function routeHosted(page, opts = {}) {
  return Promise.all([
    page.route('**/api/state', (r) => r.fulfill({ contentType: 'application/json',
      body: JSON.stringify(HOSTED_STATE) })),
    page.route('**/api/health', (r) => r.fulfill({ contentType: 'application/json',
      body: JSON.stringify({
        ok: 3, degraded: 1, absent: 4,
        components: [
          { label: 'Engine', state: 'ok', detail: 'orchestrator running' },
          { label: 'Provider', state: opts.providerOk ? 'ok' : 'degraded',
            detail: opts.providerOk ? 'at least one provider key configured'
                                    : 'no provider key - add one to reply' },
          { label: 'Memory', state: 'ok', detail: '0 memories' },
          { label: 'Storage', state: 'ok', detail: 'durable instance volume' },
          { label: 'Display', state: 'absent', detail: 'no screen on a hosted instance' },
          { label: 'Ring', state: 'absent', detail: 'no LED ring on a hosted instance' },
          { label: 'Audio', state: 'absent', detail: 'no mic or speaker on a hosted instance' },
          { label: 'Battery', state: 'absent', detail: 'on external power (hosted)' },
        ],
      }) })),
  ]);
}

// (1) Every pane switches with no thrown error under the HOSTED flag + payloads -
//     the live trigger the device-only robustness suite missed.
test('hosted: every pane switches with no thrown error', async ({ page }) => {
  const { errors } = collectErrors(page);
  await markHosted(page);
  await routeHosted(page);
  await seedToken(page);
  await openApp(page);
  for (const dest of ['home', 'chat', 'memory', 'assistant', 'device']) {
    await page.locator(`.tab[data-p=${dest}]`).click();
    await page.waitForTimeout(150);
    // Navigation actually completed: the tab is marked and Home is not stuck under it.
    await expect(page.locator(`.tab[data-p=${dest}]`)).toHaveClass(/on/);
  }
  await page.waitForTimeout(300);
  expect(errors, `hosted page threw during tab switch:\n${errors.join('\n')}`).toEqual([]);
});

// (2) The defensive guard directly: a pane loader that THROWS must not break
//     navigation. Fails on the pre-fix code (the throw escaped goDest's onclick).
test('hosted: a throwing pane loader does not break navigation', async ({ page }) => {
  const { errors } = collectErrors(page);
  await markHosted(page);
  await routeHosted(page);
  await seedToken(page);
  await openApp(page);
  // Force the Home loader to throw synchronously, the way a hosted-only absent field
  // could. loadPane('dash') calls loadHealth(); the guard must swallow it.
  await page.evaluate(() => { window.loadHealth = () => { throw new Error('boom'); }; });
  await page.locator('.tab[data-p=assistant]').click();
  await page.waitForTimeout(120);
  await page.locator('.tab[data-p=home]').click();
  await page.waitForTimeout(200);
  await expect(page.locator('#pane-dash')).toBeVisible();     // still switched
  await expect(page.locator('.tab[data-p=home]')).toHaveClass(/on/);
  expect(errors, `a throwing loader escaped navigation:\n${errors.join('\n')}`).toEqual([]);
});

// (3) Honest RAM tile: the container memory in MB, never the device "floor 34K" line.
test('hosted: the memory tile is honest (MB, no ESP heap floor)', async ({ page }) => {
  await markHosted(page);
  await routeHosted(page);
  await seedToken(page);
  await openApp(page);
  await page.waitForTimeout(300);
  const tiles = await page.locator('#devtiles').innerText();
  expect(tiles).toContain('MB');
  expect(tiles).not.toContain('floor 34K');
});

// (4) The network line ("ap undefined - undefined") is gone on a hosted instance.
test('hosted: no "ap undefined" network line', async ({ page }) => {
  await markHosted(page);
  await routeHosted(page);
  await seedToken(page);
  await openApp(page);
  await page.waitForTimeout(300);
  const info = await page.locator('#info').innerText();
  expect(info.toLowerCase()).not.toContain('ap ');
  expect(info).not.toContain('undefined');
});

// (5) Hardware controls hidden: the audio probe buttons; and the ESP OTA controls
//     replaced by an honest note. The "Software update" group STAYS a titled group in
//     its normal place (no floating orphan between Sound and Connectivity, CUM-218) but
//     its ESP-OTA controls are gone and its body states the platform manages updates.
test('hosted: ESP OTA controls become an honest platform note, audio probes hidden', async ({ page }) => {
  await markHosted(page);
  await routeHosted(page);
  await seedToken(page);
  await openApp(page);
  await page.locator('.tab[data-p=device]').click();
  await page.waitForTimeout(300);
  await expect(page.locator('#fwCheck')).toHaveCount(0);     // no "Check for Updates" control
  await expect(page.locator('#fwsec')).toContainText('managed by the platform');
  await expect(page.locator('#fwsec')).toContainText('image is rolled');
  await expect(page.locator('#fwsec')).toContainText('Software version'); // the running version is surfaced, not hidden (see-version parity)
  await expect(page.locator('#hostedFwVer')).toHaveText('v0');         // filled from d.fw on the state poll
  await expect(page.locator('#hpMic')).toHaveCount(0);       // no Mic Test button
  await expect(page.locator('#hpBeep')).toHaveCount(0);      // no Speaker Tone button
});

// (8) Honest Cloud access + sign-in (CUM-218). A hosted instance is DEFINED by its
//     cloud link, so the device-firmware pairing UI ("Cloud access is off. Pair with
//     the cloud") is a lie there. It must state the one true fact - reached through its
//     cloud link, with the tunnel URL - and drop the Pair button. The device sign-in
//     code is a LAN recovery aid that does not apply, so it is hidden, never a bare "-".
test('hosted: Cloud access states the cloud link and drops pairing + sign-in code', async ({ page }) => {
  await markHosted(page);
  await routeHosted(page);   // hosted /api/state carries no d.cloud, as nimbusd serves
  await seedToken(page);
  await openApp(page);
  await page.locator('.tab[data-p=device]').click();
  await page.waitForTimeout(200);
  // Honest cloud line + tunnel URL, and no pairing controls.
  const line = page.locator('#cloudLine');
  await expect(line).toContainText('reached through its cloud link');
  await expect(line).toContainText(page.url().replace(/\/[^/]*$/, ''));   // the tunnel URL
  await expect(page.locator('#cloudPair')).toBeHidden();
  await expect(page.locator('#cloudTip')).toBeHidden();
  // Device sign-in code (a LAN aid) is hidden in both homes, never shown as "-".
  await expect(page.locator('#idTokenRow')).toBeHidden();
  await expect(page.locator('#cxTokenRow')).toBeHidden();
});

// (7) Honest Connectivity (CUM-207/CUM-279 follow-up): a hosted instance has no radio,
//     so the whole Wi-Fi group ("Trying saved networks", the saved list, Scan, Add a
//     hidden network, Recovery) and the Bluetooth group must be gone, replaced by one
//     honest platform line. The LAN "On your network" row is dropped too. Cloud access
//     stays. Fails on the pre-fix page (the false Wi-Fi/Bluetooth groups render on a VM).
test('hosted: Wi-Fi and Bluetooth groups collapse to one honest platform line', async ({ page }) => {
  await markHosted(page);
  await routeHosted(page);
  await seedToken(page);
  await openApp(page);
  await page.locator('.tab[data-p=device]').click();
  await page.waitForTimeout(200);
  // Open the Connectivity group so its (visible) contents can be asserted - it is a
  // collapsed <details> by default, like every setgroup.
  const conn = page.locator('#pane-set > details.setgroup', {
    has: page.locator('> summary', { hasText: 'Connectivity' }),
  });
  if (!(await conn.evaluate((el) => el.open))) await conn.locator('> summary').click();
  await expect(conn.locator('> .setbody')).toBeVisible();
  await page.waitForTimeout(200);
  // Both radio groups gone; the "Trying saved networks" false line cannot show.
  await expect(page.locator('#wifiGroup')).toBeHidden();
  await expect(page.locator('#btGroup')).toBeHidden();
  await expect(page.locator('#wifiConnState')).toBeHidden();
  // The LAN address row is dropped (a hosted instance has no LAN address).
  await expect(page.locator('#cxLanRow')).toBeHidden();
  // One honest platform line stands in for both groups.
  const line = page.locator('#hostedNetLine');
  await expect(line).toBeVisible();
  await expect(line).toContainText('managed by the platform');
  await expect(line).toContainText('cloud link');
  // Cloud access is untouched - still the real way in.
  await expect(page.locator('#pane-set > details.setgroup', {
    has: page.locator('> summary', { hasText: 'Cloud access' }),
  })).toHaveCount(1);
});

// (9) Honest hosted battery/power (CUM-214 ruling): a hosted instance has no pack, no LED
//     ring, and no sense hardware, so the battery controls are false there. Since the
//     Settings IA rework (ba0b0c4) they live in ONE Battery area (#battsec, which now
//     carries the mode preset + readout) plus Customize battery mode (#custProfGroup).
//     They collapse to one honest platform line. On a VN the profile is a no-op: nimbusd
//     omits it from /api/state and /api/config ignores hardware-only keys, so hiding the
//     control hides no real knob. Fails on the pre-fix page (battery groups render on a VM).
test('hosted: battery groups collapse to one honest platform line', async ({ page }) => {
  await markHosted(page);
  await routeHosted(page);
  await seedToken(page);
  await openApp(page);
  await page.locator('.tab[data-p=device]').click();
  await page.waitForTimeout(200);
  // Both battery groups are hidden (never a dead/empty control on a VN).
  await expect(page.locator('#custProfGroup')).toBeHidden();
  await expect(page.locator('#battsec')).toBeHidden();
  // The profile radios (Dark/Balanced/Full) go with them - no dead battery-mode control.
  await expect(page.locator('input[name=profile]').first()).toBeHidden();
  // One honest platform line stands in for all three.
  const line = page.locator('#hostedBattLine');
  await expect(line).toBeVisible();
  await expect(line).toContainText('managed by the platform');
  await expect(line).toContainText('external power');
});

// (9b) Counter-test: the hide is HOSTED-gated, not a blanket removal. On a normal device
//      (no hosted flag) the Battery area (#battsec, carrying the Dark/Balanced/Full mode
//      radios since the Settings IA rework) + Customize group still render and no platform
//      line appears - so the class rule is "hosted hides these", not "these are gone".
test('device mode still shows the battery-mode groups (hide is hosted-gated)', async ({ page }) => {
  await seedToken(page);
  await openApp(page);
  await page.locator('.tab[data-p=device]').click();
  await page.waitForTimeout(200);
  await expect(page.locator('#battsec')).toBeVisible();
  // Customize now nests INSIDE the collapsed Battery area (Settings IA rework):
  // open it and the group is really there, radios and all.
  await page.locator('#battsec > summary').click();
  await expect(page.locator('#custProfGroup')).toBeVisible();
  await expect(page.locator('input[name=profile]').first()).toBeVisible();
  await expect(page.locator('#hostedBattLine')).toHaveCount(0);
});

// (6) Provider-degraded resolve link (item 2): the Health row links to Providers.
test('hosted: a degraded provider row offers a resolve link', async ({ page }) => {
  await markHosted(page);
  await routeHosted(page, { providerOk: false });
  await seedToken(page);
  await openApp(page);
  await page.waitForTimeout(300);
  const fix = page.locator('#healthlist [data-fix=llm]');
  await expect(fix).toBeVisible();
  await fix.click();   // jumps to the Assistant > Models subtab
  await expect(page.locator('.subtab[data-sp=llm]')).toHaveClass(/on/);
});

// (7) No control that only answers an error: Restart and Power off are the platform's.
test('hosted: Home and Device show no restart or power control', async ({ page }) => {
  await markHosted(page);
  await routeHosted(page);
  await seedToken(page);
  await openApp(page);
  await expect(page.locator('#homeRestart')).toBeHidden();
  await expect(page.locator('#homePowerOff')).toBeHidden();
  await page.locator('.tab[data-p=device]').click();
  await expect(page.locator('#deviceRestart')).toBeHidden();
  await expect(page.locator('#powerOff')).toBeHidden();
  await expect(page.locator('#modehdr')).toBeHidden();
  // A device-only group is gone; what a hosted instance really has stays.
  const groups = await page.locator('#pane-device details.setgroup:visible > summary, details.setgroup:visible > summary').allTextContents();
  for (const gone of ['Display', 'Ring', 'Sound', 'Power', 'Danger zone'])
    expect(groups.some((t) => t.startsWith(gone))).toBe(false);
  expect(groups.some((t) => t.startsWith('Software update'))).toBe(true);
});

// (8) A spawn-only turn answers done with no reply (the server half of the fix): the
//     placeholder goes and the next message is sent. This passes on a page from before the
//     fix too: it pins the contract. The page half is the next spec.
test('hosted: a spawn-only turn frees the queue', async ({ page }) => {
  await markHosted(page);
  await routeHosted(page);
  await seedToken(page);
  let landed = false;
  let watches = 0;
  const posts = [];
  await page.route('**/api/mem/episodic**', (r) => {
    const ms = [{ id: 'm1', session: 'owner', channel: 'web', role: 'user', kind: 'message', text: 'start it', tags: '' }];
    if (landed) ms.unshift({ id: 'm2', session: 'owner', channel: 'web', role: 'assistant', kind: 'message', text: 'RESULT: 144', tags: '' });
    return r.fulfill({ contentType: 'application/json', body: JSON.stringify({ messages: ms }) });
  });
  await page.route('**/api/chat**', (route) => {
    const req = route.request();
    const json = (b) => route.fulfill({ contentType: 'application/json', body: JSON.stringify(b) });
    if (req.method() === 'POST') {
      posts.push(req.postData());
      return json({ pending: true, turn: posts.length });
    }
    const u = new URL(req.url());
    if (u.searchParams.get('watch')) {
      watches += 1;
      if (watches >= 2) landed = true;
      return json({ async: landed ? 2 : 1, busy: landed ? 0 : 1 });
    }
    return json({ reply: '', pending: false, done: true, async: 1, busy: 1 });
  });
  await openApp(page);
  await page.locator('.tab[data-p=chat]').click();
  await page.locator('#chatInput').fill('start it');
  await page.locator('#chatSend').click();
  await page.locator('#chatInput').fill('and this');
  await page.locator('#chatSend').click();
  await expect.poll(() => posts.length, { timeout: 8000 }).toBe(2);
  await expect(page.locator('#chatLog')).toContainText('RESULT: 144', { timeout: 15000 });
});

// (8b) The page half: a page opened or reloaded while a sub-agent runs watches for its
//      result by itself, with no message sent from it. The hosted page used not to watch.
test('hosted: a page opened during background work shows its result with no message sent', async ({ page }) => {
  await markHosted(page);
  await routeHosted(page);
  await seedToken(page);
  let landed = false;
  let watches = 0;
  await page.route('**/api/mem/episodic**', (r) => {
    const ms = [{ id: 'm1', session: 'owner', channel: 'web', role: 'user', kind: 'message', text: 'start it', tags: '' }];
    if (landed) ms.unshift({ id: 'm2', session: 'owner', channel: 'web', role: 'assistant', kind: 'message', text: 'RESULT: 144', tags: '' });
    return r.fulfill({ contentType: 'application/json', body: JSON.stringify({ messages: ms }) });
  });
  await page.route('**/api/chat**', (route) => {
    const u = new URL(route.request().url());
    expect(route.request().method()).toBe('GET');   // nothing is sent from this page
    if (u.searchParams.get('watch')) {
      watches += 1;
      if (watches >= 2) landed = true;
      return route.fulfill({ contentType: 'application/json', body: JSON.stringify({ async: landed ? 2 : 1, busy: landed ? 0 : 1 }) });
    }
    return route.fulfill({ contentType: 'application/json', body: '{"gone":true}' });
  });
  await openApp(page);
  await page.locator('.tab[data-p=chat]').click();
  await expect(page.locator('#chatLog')).toContainText('RESULT: 144', { timeout: 20000 });
  expect(watches).toBeGreaterThanOrEqual(2);
});

// (9) A photo goes with the next message on a hosted page, named by its Files entry.
test('hosted: an attached photo goes with the next message', async ({ page }) => {
  await markHosted(page);
  await routeHosted(page);
  await seedToken(page);
  let sent = '';
  await page.route('**/api/files/upload**', (r) => r.fulfill({ contentType: 'application/json', body: '{"ok":true}' }));
  await page.route('**/api/chat**', (route) => {
    const req = route.request();
    if (req.method() === 'POST') sent = req.postData() || '';
    return route.fulfill({ contentType: 'application/json',
      body: JSON.stringify(req.method() === 'POST' ? { pending: true, turn: 1 } : { reply: 'green', pending: false, done: true, async: 0, busy: 0 }) });
  });
  await openApp(page);
  await page.locator('.tab[data-p=chat]').click();
  await page.setInputFiles('#chatFile', { name: 'p.png', mimeType: 'image/png', buffer: Buffer.from('89504e470d0a1a0a', 'hex') });
  await expect(page.locator('#chatLog')).toContainText('It goes with your next message');
  await page.locator('#chatInput').fill('what color?');
  await page.locator('#chatSend').click();
  await expect.poll(() => sent, { timeout: 8000 }).toContain('att=chat%2Fatt-');
});

// (9b) Whatever a browser names a photo, Files takes it: printable ASCII, no space, at
//      most 48 characters, and each one its own entry, so two photos never overwrite.
test('hosted: any photo name uploads under a safe, unique Files name', async ({ page }) => {
  await markHosted(page);
  await routeHosted(page);
  await seedToken(page);
  const names = [];
  let sent = '';
  await page.route('**/api/files/upload**', (r) => {
    names.push(new URL(r.request().url()).searchParams.get('name'));
    return r.fulfill({ contentType: 'application/json', body: '{"ok":true}' });
  });
  await page.route('**/api/chat**', (route) => {
    const req = route.request();
    if (req.method() === 'POST') sent = req.postData() || '';
    return route.fulfill({ contentType: 'application/json',
      body: JSON.stringify(req.method() === 'POST' ? { pending: true, turn: 1 } : { reply: 'ok', pending: false, done: true, async: 0, busy: 0 }) });
  });
  await openApp(page);
  await page.locator('.tab[data-p=chat]').click();
  const png = { mimeType: 'image/png', buffer: Buffer.from('89504e470d0a1a0a', 'hex') };
  await page.setInputFiles('#chatFile', [
    { name: 'Screenshot 2026-11-02 at 14.33.12.png', ...png },
    { name: 'image.png', ...png },
    { name: 'image.png', ...png },
    { name: 'фото отпуск.png', ...png },
    { name: `${'a-very-long-name-'.repeat(8)}.png`, ...png },
  ]);
  await expect.poll(() => names.length, { timeout: 8000 }).toBe(5);
  await expect(page.locator('#chatLog')).toContainText('Attached Screenshot 2026-11-02 at 14.33.12.png');
  for (const n of names) {
    expect(n).toMatch(/^att-[\x21-\x7e]+$/);
    expect(n.length).toBeLessThanOrEqual(48);
    expect(n).not.toMatch(/[/\\]/);
    expect(n).toMatch(/\.png$/);
  }
  expect(new Set(names).size).toBe(5);
  await page.locator('#chatInput').fill('what are these?');
  await page.locator('#chatSend').click();
  await expect.poll(() => sent, { timeout: 8000 }).toContain('att=');
  const att = new URLSearchParams(sent).get('att').split('\n');
  expect(att).toEqual(names.map((n) => `chat/${n}`));
});

// (9c) A file that is no photo keeps its name where it can, in the form Files takes.
test('hosted: a plain file with spaces in its name uploads under a safe name', async ({ page }) => {
  await markHosted(page);
  await routeHosted(page);
  await seedToken(page);
  const names = [];
  await page.route('**/api/files/upload**', (r) => {
    names.push(new URL(r.request().url()).searchParams.get('name'));
    return r.fulfill({ contentType: 'application/json', body: '{"ok":true}' });
  });
  await openApp(page);
  await page.locator('.tab[data-p=chat]').click();
  await page.setInputFiles('#chatFile', { name: 'my tax notes 2026.txt', mimeType: 'text/plain', buffer: Buffer.from('hello') });
  await expect.poll(() => names.length, { timeout: 8000 }).toBe(1);
  expect(names[0]).toBe('my-tax-notes-2026.txt');
  await expect(page.locator('#chatLog')).toContainText('in Files > chat');
});

// (9d) A full store says so: the server's own sentence, not "check the file".
test('hosted: a refused photo upload shows why', async ({ page }) => {
  await markHosted(page);
  await routeHosted(page);
  await seedToken(page);
  await page.route('**/api/files/upload**', (r) => r.fulfill({ status: 400, contentType: 'application/json',
    body: JSON.stringify({ ok: false, error: "Couldn't store the file: too many files (remove some first)." }) }));
  await openApp(page);
  await page.locator('.tab[data-p=chat]').click();
  await page.setInputFiles('#chatFile', { name: 'p.png', mimeType: 'image/png', buffer: Buffer.from('89504e470d0a1a0a', 'hex') });
  await expect(page.locator('#chatUpMsg')).toContainText('too many files');
});

// (10) A read during a turn answers a plain 503. The page waits and asks again: a chat
//      opened in that moment used to stay empty until the next message.
test('hosted: a busy history read is retried, and a photo row reads as a photo', async ({ page }) => {
  await markHosted(page);
  await routeHosted(page);
  await seedToken(page);
  let reads = 0;
  await page.route('**/api/mem/episodic**', (r) => {
    reads += 1;
    if (reads === 1) return r.fulfill({ status: 503, contentType: 'application/json', body: '{"error":"instance busy - try again"}' });
    return r.fulfill({ contentType: 'application/json', body: JSON.stringify({ messages: [
      { id: 'm2', session: 'owner', channel: 'web', role: 'user', kind: 'message', tags: '',
        text: '[image 1/1 ref=add8683db7bb777d] They wrote: What color?' },
    ] }) });
  });
  await page.route('**/api/chat**', (r) => r.fulfill({ contentType: 'application/json', body: '{"async":0,"busy":0}' }));
  await openApp(page);
  await page.locator('.tab[data-p=chat]').click();
  await expect(page.locator('#chatLog')).toContainText('[photo] What color?', { timeout: 15000 });
  await expect(page.locator('#chatLog')).not.toContainText('add8683db7bb777d');
});

// (11) Assistant, Memory and the search box offer nothing the hosted server refuses or
//      ignores. tools/persona-rig/vn_browser_check.py walks the same class on a real
//      instance: it changes every control it can see and reads the server's answer.
test('hosted: Assistant shows no control the instance refuses or ignores', async ({ page }) => {
  await markHosted(page);
  await routeHosted(page);
  await seedToken(page);
  await openApp(page);
  await page.locator('.tab[data-p=assistant]').click();
  await page.waitForTimeout(200);
  const open = async () => page.evaluate(() => document.querySelectorAll('details').forEach((d) => { d.open = true; }));
  const groups = async () => (await page.locator('details.setgroup:visible > summary').allTextContents()).map((t) => t.trim());
  await page.locator('.subtab[data-sp=llm]').click();
  await open();
  const models = await groups();
  expect(models.some((t) => t.startsWith('Providers'))).toBe(true);
  for (const gone of ['Voice', 'Custom endpoint']) expect(models.some((t) => t.startsWith(gone))).toBe(false);
  await expect(page.locator('#provPrioList')).not.toContainText('Custom');
  await expect(page.locator('#orchHost option[value=custom]')).toHaveCount(0);
  await page.locator('.subtab[data-sp=connectors]').click();
  await open();
  await expect(page.locator('#ttsOn')).toBeHidden();
  await expect(page.locator('#tgToken')).toBeHidden();
  await expect(page.locator('#tgAddId')).toBeVisible();
  await page.locator('.subtab[data-sp=skills]').click();
  await expect(page.locator('#skAuto')).toBeHidden();
  await expect(page.locator('#skSave')).toBeVisible();
  await page.locator('.subtab[data-sp=tools]').click();
  await open();
  for (const id of ['midFail', 'loopRounds', 'loopDeadline', 'compactKB', 'tlsSlots', 'tlsVerify', 'capProbe', 'capProbeH'])
    await expect(page.locator(`#${id}`), id).toBeHidden();
  await expect(page.locator('#orchLoop')).toBeVisible();
  await expect(page.locator('#orchTrace')).toBeVisible();
  await expect(page.locator('.subtab[data-sp=safety]')).toBeHidden();
  await page.locator('.tab[data-p=memory]').click();
  await open();
  await expect(page.locator('#prefsnew')).toBeHidden();
  await expect(page.locator('#clearconv')).toBeVisible();
  // The search box holds no way to a group that is gone.
  await page.keyboard.press('Control+k');
  await page.locator('#searchOverlay input').fill('erase');
  await page.waitForTimeout(150);
  await expect(page.locator('#searchOverlay')).not.toContainText('factory reset');
});

// (10b) A turn nobody typed: what started it is a quiet line, not an assistant bubble, and
//       the reply keeps its summary chip. The shared page, so the device reads the same.
test('a sub-agent result turn shows its trigger as a line and its chip on the reply', async ({ page }) => {
  await markHosted(page);
  await routeHosted(page);
  await seedToken(page);
  await page.route('**/api/mem/episodic**', (r) => r.fulfill({ contentType: 'application/json', body: JSON.stringify({ messages: [
    { id: 'm3', session: 'owner', channel: 'web', role: 'system', kind: 'log', tags: 'ev:turnend,turn:m1',
      text: JSON.stringify({ host: 'mistral', model: 'mistral-small', ok: true, in: 10, out: 5, tools: 0 }) },
    { id: 'm2', session: 'owner', channel: 'web', role: 'assistant', kind: 'message', tags: 'out:reply,turn:m1', text: 'It is 144.' },
    { id: 'm1', session: 'owner', channel: 'web', role: 'system', kind: 'message', tags: 'ev:trigger,src:synth,turn:m1', text: '[sub-agent results arrived]' },
  ] }) }));
  await page.route('**/api/chat**', (r) => r.fulfill({ contentType: 'application/json', body: '{"async":0,"busy":0}' }));
  await openApp(page);
  await page.locator('.tab[data-p=chat]').click();
  await expect(page.locator('#chatLog')).toContainText('It is 144.', { timeout: 8000 });
  await expect(page.locator('#chatLog')).toContainText('sub-agent results arrived');
  await expect(page.locator('#chatLog')).not.toContainText('[sub-agent results arrived]');
  const bubbles = await page.locator('#chatLog > *').allInnerTexts();
  const reply = bubbles.find((t) => t.includes('It is 144.'));
  expect(reply).toContain('mistral');
});

// (11a) The lines that would be untrue here read in the instance's own words.
test('hosted: Memory, Chat and Device make no claim about a card, a mic or Wi-Fi', async ({ page }) => {
  await markHosted(page);
  await routeHosted(page);
  await seedToken(page);
  await page.route('**/api/mem/episodic**', (r) => r.fulfill({ contentType: 'application/json', body: '{"messages":[]}' }));
  await openApp(page);
  await page.locator('.tab[data-p=chat]').click();
  await expect(page.locator('#chatLog')).toContainText('No conversation yet', { timeout: 8000 });
  await expect(page.locator('#chatLog')).not.toContainText('mic');
  await page.locator('.tab[data-p=memory]').click();
  const mem = await page.locator('#pane-mem').innerText();
  expect(mem).toContain('stored on its own volume');
  expect(mem).not.toContain('SD card');
  await page.locator('.tab[data-p=device]').click();
  const dev = await page.locator('#pane-set').textContent();
  expect(dev).toContain('Set by the platform');
  expect(dev).not.toContain('once Wi-Fi connects');
});

// (11b) Counter-test: the hide is hosted-gated. The device keeps every one of them.
test('device mode keeps the controls a hosted instance hides', async ({ page }) => {
  await seedToken(page);
  await openApp(page);
  await page.locator('.tab[data-p=assistant]').click();
  await page.evaluate(() => document.querySelectorAll('details').forEach((d) => { d.open = true; }));
  await page.locator('.subtab[data-sp=llm]').click();
  await expect(page.locator('#sttProv')).toBeVisible();
  await expect(page.locator('#custBase')).toBeVisible();
  await page.locator('.subtab[data-sp=skills]').click();
  await expect(page.locator('#skAuto')).toBeVisible();
  await page.locator('.subtab[data-sp=tools]').click();
  await expect(page.locator('#loopRounds')).toBeVisible();
  await expect(page.locator('.subtab[data-sp=safety]')).toBeVisible();
  await expect(page.locator('#orchTrace').locator('xpath=..')).toContainText('thinking');
});

// (12) A web search key on a hosted instance: saved, and nothing promises a check that
//      never comes, however often the page reloads.
test('hosted: a saved web search key never reads as verifying', async ({ page }) => {
  await markHosted(page);
  await routeHosted(page);
  await seedToken(page);
  await openApp(page);
  const orch = await page.evaluate(() => fetch('/api/orch').then((r) => r.json()));
  await page.route('**/api/orch', (r) => r.fulfill({ contentType: 'application/json',
    body: JSON.stringify({ ...orch, hasTav: true, tavVerify: undefined, tavVts: undefined }) }));
  await page.locator('.tab[data-p=assistant]').click();
  await page.locator('.subtab[data-sp=tools]').click();
  await expect(page.locator('#tavstat')).toContainText('Key saved', { timeout: 8000 });
  await expect(page.locator('#tavstat')).not.toContainText('verifying');
  await expect(page.locator('#tavstat')).not.toContainText('not yet verified');
});

// (12b) The same for a connected bot: live, with no check that never lands.
test('hosted: a connected bot never reads as verifying', async ({ page }) => {
  await markHosted(page);
  await routeHosted(page);
  await seedToken(page);
  await openApp(page);
  const orch = await page.evaluate(() => fetch('/api/orch').then((r) => r.json()));
  await page.route('**/api/orch', (r) => r.fulfill({ contentType: 'application/json',
    body: JSON.stringify({ ...orch, hasTg: true, tgLive: true, tgVerify: undefined, tgVts: undefined }) }));
  await page.locator('.tab[data-p=assistant]').click();
  await page.locator('.subtab[data-sp=connectors]').click();
  await page.evaluate(() => document.querySelectorAll('details').forEach((d) => { d.open = true; }));
  await expect(page.locator('#tgReady')).toContainText('Bot active', { timeout: 8000 });
  await expect(page.locator('#tgReady')).not.toContainText('erifying');
  await expect(page.locator('#tgReady')).not.toContainText('voice notes');
  await expect(page.locator('#tgReady')).toContainText('Reads text and photos');
});

// (13) The recording line tells the truth in both states, and off is reachable.
test('hosted: the chat says recording is off only when it is, and the box saves', async ({ page }) => {
  await markHosted(page);
  await routeHosted(page);
  await seedToken(page);
  await openApp(page);
  const base = await page.evaluate(() => fetch('/api/orch').then((r) => r.json()));
  let trace = true;
  const posted = [];
  await page.route('**/api/orch', (r) => {
    if (r.request().method() === 'POST') {
      const body = r.request().postData() || '';
      posted.push(body);
      if (/orchTrace=0/.test(body)) trace = false;
      return r.fulfill({ contentType: 'application/json', body: '{"ok":true}' });
    }
    return r.fulfill({ contentType: 'application/json', body: JSON.stringify({ ...base, orchTrace: trace }) });
  });
  await page.locator('.tab[data-p=chat]').click();
  await page.waitForTimeout(600);
  await expect(page.locator('#chatTrace')).toBeHidden();
  await page.locator('.tab[data-p=assistant]').click();
  await page.locator('.subtab[data-sp=tools]').click();
  await page.evaluate(() => document.querySelectorAll('details').forEach((d) => { d.open = true; }));
  const box = page.locator('#orchTrace');
  await expect(box).toBeChecked({ timeout: 8000 });
  const row = box.locator('xpath=..');
  await expect(row).toContainText('Record tool calls');
  await expect(row).not.toContainText('thinking');
  await box.uncheck();
  await page.locator('#toolusesave').click();
  await expect.poll(() => posted.length, { timeout: 8000 }).toBeGreaterThan(0);
  expect(posted[posted.length - 1]).toContain('orchTrace=0');
  for (const fixed of ['midFail', 'loopRounds', 'tlsSlots', 'custBase', 'tgToken']) expect(posted[posted.length - 1]).not.toContain(fixed);
  await page.locator('.tab[data-p=chat]').click();
  await expect(page.locator('#chatTrace')).toContainText('Activity recording is off', { timeout: 8000 });
  await expect(page.locator('#chatTrace')).toContainText('Assistant');
});
