// Busy answers render as "busy, retrying" (nimbus/sys/web_guard.h): while a tool holds
// the SD card, every card-backed endpoint answers 503 with Retry-After and
// {"error":"busy"}. A page that loads one keeps its last view, shows a busy note above
// the section, and loads again after Retry-After. It never shows the busy answer as
// data: no "No SD card", no "Embedding error: busy", no "unavailable", no empty list.
// Downloads wait out a busy answer and then complete. A 503 with no Retry-After (the
// archive needs the SD card) is an answer, not busy (the counter-test).
import { test, expect } from '@playwright/test';
import { seedToken, openApp, assertPane } from './_helpers.mjs';

const BUSY_TEXT = 'The SD card is busy right now. Retrying in a few seconds.';
const CARD_ROUTES = ['**/api/mem/stats', '**/api/mem/vector**', '**/api/mem/nsusage', '**/api/mem/scratchpad',
  '**/api/prefs', '**/api/files/list**', '**/api/safety', '**/api/health', '**/api/tools', '**/api/skills/list'];

function busyAnswer(route) {
  return route.fulfill({ status: 503, contentType: 'application/json', headers: { 'Retry-After': '2' },
    body: '{"error":"busy"}' });
}

// Every card route answers busy while st.busy; then the mock server answers.
async function routeBusy(page, st) {
  for (const pat of CARD_ROUTES)
    await page.route(pat, (r) => (st.busy && r.request().method() === 'GET' ? busyAnswer(r) : r.fallback()));
}

async function shot(page, info, name, sel) {
  const target = sel ? page.locator(sel).first() : page;
  await target.screenshot({ path: `screenshots/${info.project.name}-busy-${name}.png` });
}

test('memory and files: a busy card shows busy, retrying, never a false empty or error state', async ({ page }, info) => {
  await seedToken(page);
  const st = { busy: true };
  await routeBusy(page, st);
  await openApp(page);
  await page.locator('.tab[data-p=memory]').click();
  await assertPane(page, 'memory');
  for (const g of ['Long-term memory', 'Files'])
    await page.locator('#pane-mem details.setgroup > summary', { hasText: g }).click();
  for (const id of ['tierbanner', 'memlist', 'nsUsage', 'scratchview', 'filesList'])
    await expect(page.locator(`#pane-mem .cardbusy + #${id}`), id).toHaveCount(1);
  const note = page.locator('#pane-mem .cardbusy:has(+ #tierbanner)');
  await expect(note).toContainText(BUSY_TEXT);
  await expect(note).toBeVisible();
  // Never the busy answer read as data.
  await expect(page.locator('#tierbanner')).toBeHidden();
  await expect(page.locator('#memvmsg')).not.toContainText('Embedding error');
  await expect(page.locator('#memlist')).not.toContainText('No memories');
  await expect(page.locator('#nsUsage')).not.toContainText('unavailable');
  await expect(page.locator('#filesList')).not.toContainText('unavailable');
  await expect(page.locator('#filesStat')).not.toContainText('No SD card');
  await expect(page.locator('#memstat')).toHaveText('SD card busy');
  await shot(page, info, 'memory', '#pane-mem');
  // The card frees: the notes go and the data shows, with no reload.
  st.busy = false;
  await expect(page.locator('#pane-mem .cardbusy')).toHaveCount(0, { timeout: 10000 });
  await expect(page.locator('#memstat')).toContainText('memories');
  await expect(page.locator('#filesList')).toContainText('notes.txt');
  await shot(page, info, 'memory-recovered', '#pane-mem');
});

test('a busy poll keeps the last view: the files list stays while the card is held', async ({ page }) => {
  await seedToken(page);
  const st = { busy: false };
  await routeBusy(page, st);
  await openApp(page);
  await page.locator('.tab[data-p=memory]').click();
  await page.locator('#pane-mem details.setgroup > summary', { hasText: 'Files' }).click();
  await expect(page.locator('#filesList')).toContainText('notes.txt');
  st.busy = true;
  await page.locator('#filesRefresh').click();
  await expect(page.locator('#pane-mem .cardbusy + #filesList')).toHaveCount(1);
  await expect(page.locator('#filesList')).toContainText('notes.txt');   // the last view stays
  st.busy = false;
  await expect(page.locator('#pane-mem .cardbusy')).toHaveCount(0, { timeout: 10000 });
});

test('safety activity: busy, retrying, then the entries', async ({ page }, info) => {
  await seedToken(page);
  const st = { busy: true };
  await routeBusy(page, st);
  await openApp(page);
  await page.locator('.tab[data-p=assistant]').click();
  await page.locator('.subtab[data-sp=safety]').click();
  await expect(page.locator('#subpane-safety .cardbusy + #safetyList')).toHaveCount(1);
  await expect(page.locator('#subpane-safety .cardbusy')).toContainText(BUSY_TEXT);
  await expect(page.locator('#safetyList')).not.toContainText('Nothing flagged');
  await expect(page.locator('#safetyList')).not.toContainText('Could not load');
  await expect(page.locator('#safetyAllow')).not.toContainText('None yet');
  await shot(page, info, 'safety', '#subpane-safety');
  st.busy = false;
  await expect(page.locator('#subpane-safety .cardbusy')).toHaveCount(0, { timeout: 10000 });
  await expect(page.locator('#safetyList')).not.toContainText('loading');
});

test('home health and assistant tools: busy, retrying, then the data', async ({ page }, info) => {
  await seedToken(page);
  const st = { busy: true };
  await routeBusy(page, st);
  await openApp(page);
  await expect(page.locator('#healthpanel > .cardbusy')).toBeVisible();   // inside the empty card
  await shot(page, info, 'home-health', '#pane-dash');
  await page.locator('.tab[data-p=assistant]').click();
  await page.locator('.subtab[data-sp=tools]').click();
  await expect(page.locator('.cardbusy + #toollist')).toHaveCount(1);
  await expect(page.locator('#toolstat')).not.toHaveText('Unavailable');
  st.busy = false;
  await expect(page.locator('.cardbusy + #toollist')).toHaveCount(0, { timeout: 10000 });
  await page.locator('.tab[data-p=home]').click();
  await expect(page.locator('#pane-dash .cardbusy')).toHaveCount(0, { timeout: 12000 });
  await expect(page.locator('#hphead')).toBeVisible();
});

test('a file download waits out a busy answer, says so, then saves', async ({ page }) => {
  await seedToken(page);
  let dl = 0;
  await page.route('**/api/files/dl**', (r) => (++dl === 1 ? busyAnswer(r)
    : r.fulfill({ status: 200, contentType: 'text/plain', body: 'hello' })));
  await openApp(page);
  await page.locator('.tab[data-p=memory]').click();
  await page.locator('#pane-mem details.setgroup > summary', { hasText: 'Files' }).click();
  const saved = page.waitForEvent('download');
  await page.locator('#filesList a[data-dln="notes.txt"]').click();
  await expect(page.locator('#toast')).toContainText('Waiting for the SD card');
  expect((await saved).suggestedFilename()).toBe('notes.txt');
  expect(dl).toBe(2);
});

const CHAT = '123456789', DAY = '2026-10-21';
async function routeArchive(page, st) {
  await page.route('**/api/tg/archive**', (r) => {
    const u = new URL(r.request().url());
    if (st.noCard) return r.fulfill({ status: 503, contentType: 'application/json',
      body: '{"error":"The archive needs the SD card."}' });
    if (u.pathname.endsWith('/zip')) {
      if (st.zipBusy-- > 0) return busyAnswer(r);
      return r.fulfill({ status: 200, contentType: 'application/zip', body: Buffer.from('PK\x05\x06' + '\0'.repeat(18)) });
    }
    if (st.busy) return busyAnswer(r);
    const chat = u.searchParams.get('chat'), day = u.searchParams.get('day');
    let body;
    if (!chat) body = { stats: {}, chats: [{ chat: CHAT, days: 1, newest: DAY }] };
    else if (!day) body = { chat, days: [DAY], from: 0, next: 1, more: false };
    else body = { chat, day, rows: [], from: 0, next: 0, more: false };
    return r.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(body) });
  });
}

test('archive: busy, retrying, then the chats; an export waits out a busy answer', async ({ page }, info) => {
  await seedToken(page);
  const st = { busy: true, zipBusy: 1 };
  await routeArchive(page, st);
  await openApp(page);
  await page.locator('.tab[data-p=memory]').click();
  await page.locator('#tgArcGroup summary').click();
  await expect(page.locator('#tgArcGroup .cardbusy + #tgArcStat')).toHaveCount(1);
  await expect(page.locator('#tgArcStat')).not.toContainText('Couldn');
  await shot(page, info, 'archive', '#tgArcGroup');
  st.busy = false;
  await expect(page.locator('#tgArcChat')).toHaveValue(CHAT, { timeout: 10000 });
  await expect(page.locator('#tgArcGroup .cardbusy')).toHaveCount(0);
  const saved = page.waitForEvent('download');
  await page.locator('#tgArcExport').click();
  await expect(page.locator('#toast')).toContainText('Waiting for the SD card');
  expect((await saved).suggestedFilename()).toBe(`${CHAT}-${DAY}.zip`);
});

test('a 503 with no Retry-After is an answer, not busy (the counter-test)', async ({ page }) => {
  await seedToken(page);
  await routeArchive(page, { noCard: true });
  await openApp(page);
  await page.locator('.tab[data-p=memory]').click();
  await page.locator('#tgArcGroup summary').click();
  await expect(page.locator('#tgArcStat')).toContainText('Couldn\'t load the archive');
  await expect(page.locator('#tgArcGroup .cardbusy')).toHaveCount(0);
});

test('chat history: busy, retrying, then the conversation', async ({ page }, info) => {
  await seedToken(page);
  let busy = true;
  const msg = (id, role, text) => ({ id, session: 'web', kind: 'message', role, text, tags: '' });
  await page.route('**/api/mem/episodic**', (r) => (busy ? busyAnswer(r)
    : r.fulfill({ status: 200, contentType: 'application/json',
      body: JSON.stringify({ messages: [msg('m2', 'assistant', 'hello back'), msg('m1', 'user', 'hello')],
        total: 2, indexComplete: true, olderExists: false }) })));
  await openApp(page);
  await page.locator('.tab[data-p=chat]').click();
  await expect(page.locator('#chatDrop .cardbusy + #chatLog')).toHaveCount(1);
  await expect(page.locator('#chatLog')).not.toContainText('No conversation yet');
  await shot(page, info, 'chat', '#pane-chat');
  busy = false;
  await expect(page.locator('#chatLog')).toContainText('hello back', { timeout: 10000 });
  await expect(page.locator('#chatDrop .cardbusy')).toHaveCount(0);
});

test('an archive photo view waits out a busy answer, then shows the photo', async ({ page }) => {
  await seedToken(page);
  const PNG = Buffer.from('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==', 'base64');
  let item = 0;
  await page.route('**/api/tg/archive**', (r) => {
    const u = new URL(r.request().url());
    if (u.pathname.endsWith('/item'))
      return ++item === 1 ? busyAnswer(r) : r.fulfill({ status: 200, contentType: 'image/png', body: PNG });
    const chat = u.searchParams.get('chat'), day = u.searchParams.get('day');
    let body;
    if (!chat) body = { stats: {}, chats: [{ chat: CHAT, days: 1, newest: DAY }] };
    else if (!day) body = { chat, days: [DAY], from: 0, next: 1, more: false };
    else body = { chat, day, rows: [{ v: 1, dir: 'in', u: 7, chat, date: 1792990800, kind: 'photo', file: '7.jpg', st: 'ok' }],
      from: 0, next: 1, more: false };
    return r.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(body) });
  });
  await openApp(page);
  await page.locator('.tab[data-p=memory]').click();
  await page.locator('#tgArcGroup summary').click();
  await page.locator('.tgArcRow button', { hasText: 'View' }).click();
  await expect(page.locator('#toast')).toContainText('Waiting for the SD card');
  await expect(page.locator('#tgArcView img')).toHaveAttribute('src', /^blob:/, { timeout: 10000 });
  expect(item).toBe(2);
});

test('skills: the list and a skill load read a busy answer as busy', async ({ page }) => {
  await seedToken(page);
  const st = { busy: true };
  await routeBusy(page, st);
  await page.route('**/api/skills/get**', (r) => busyAnswer(r));
  await openApp(page);
  await page.locator('.tab[data-p=assistant]').click();
  await page.locator('.subtab[data-sp=skills]').click();
  await expect(page.locator('.cardbusy + #skList')).toHaveCount(1);
  await expect(page.locator('#skList')).not.toContainText('unavailable');
  await page.locator('#skId').fill('morning');
  await page.locator('#skLoad').click();
  await expect(page.locator('.cardbusy + #skMsg')).toHaveCount(1);
  await expect(page.locator('#skMsg')).not.toContainText('Not found');
});
