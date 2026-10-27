// The Telegram archive view (Memory): browse by chat and day, open an item, and
// export a day as a ZIP. Every read goes through the token-carrying fetch shim; row
// fields from the archive are shown as text, never markup.
import { test, expect } from '@playwright/test';
import { seedToken, openApp } from './_helpers.mjs';

const CHAT = '123456789', DAY = '2026-10-21';
const ROWS = [
  { v: 1, dir: 'in', u: 1001, chat: CHAT, date: 1792990800, kind: 'photo', raw: '1001.json', file: '1001.jpg', st: 'ok' },
  { v: 1, ev: 'media', ref: '1001', file: '1001.jpg', st: 'ok', bytes: 2048 },
  { v: 1, dir: 'out', s: 3, chat: CHAT, date: 1792990900, kind: '<img src=x onerror=alert(1)>', raw: 'o3.json', st: 'ok' },
];
const PNG = Buffer.from('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAEhQGAhKmMIQAAAABJRU5ErkJggg==', 'base64');

async function routeArchive(page, seen) {
  await page.route('**/api/tg/archive**', (r) => {
    const u = new URL(r.request().url());
    seen.push(u.pathname + u.search);
    if (u.pathname.endsWith('/zip'))
      return r.fulfill({ status: 200, contentType: 'application/zip', body: Buffer.from('PK\x05\x06' + '\0'.repeat(18)) });
    if (u.pathname.endsWith('/item')) {
      const name = u.searchParams.get('name');
      if (name.endsWith('.jpg')) return r.fulfill({ status: 200, contentType: 'image/png', body: PNG });
      return r.fulfill({ status: 200, contentType: 'text/plain', body: '{"message":{"text":"<b>hello</b>"}}' });
    }
    const chat = u.searchParams.get('chat'), day = u.searchParams.get('day');
    let body;
    if (!chat) body = { stats: {}, chats: [{ chat: CHAT, days: 2, newest: DAY }] };
    else if (!day) body = { chat, days: [DAY, '2026-10-20'], from: 0, next: 2, more: false };
    else body = { chat, day, rows: ROWS, from: 0, next: 3, more: false };
    return r.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(body) });
  });
}

async function openArchive(page) {
  await openApp(page);
  await page.locator('.tab[data-p=memory]').click();
  await page.locator('#tgArcGroup summary').click();
  await expect(page.locator('#tgArcChat')).toHaveValue(CHAT);
}

test('browse: chats, then days, then the day\'s items as text', async ({ page }) => {
  await seedToken(page);
  const seen = [];
  await routeArchive(page, seen);
  await openArchive(page);
  await expect(page.locator('#tgArcDay')).toHaveValue(DAY);
  await expect(page.locator('.tgArcRow')).toHaveCount(2);   // the media event rides its item
  await expect(page.locator('.tgArcRow').first()).toContainText('received photo');
  // Archive fields are data: the markup-shaped kind is shown as text, never parsed.
  await expect(page.locator('.tgArcRow').nth(1)).toContainText('<img src=x onerror=alert(1)>');
  await expect(page.locator('#tgArcRows img')).toHaveCount(0);
  expect(seen.some((s) => s.includes(`chat=${CHAT}&day=${DAY}`))).toBe(true);
});

test('open: a record shows as text, a photo shows in place', async ({ page }) => {
  await seedToken(page);
  const seen = [];
  await routeArchive(page, seen);
  await openArchive(page);
  await page.locator('.tgArcRow').first().locator('button', { hasText: 'Open' }).click();
  await expect(page.locator('#tgArcView pre')).toHaveText('{"message":{"text":"<b>hello</b>"}}');
  await expect(page.locator('#tgArcView b')).toHaveCount(0);
  await page.locator('.tgArcRow').first().locator('button', { hasText: 'View' }).click();
  await expect(page.locator('#tgArcView img')).toHaveAttribute('src', /^blob:/);
  expect(seen.some((s) => s.startsWith('/api/tg/archive/item') && s.includes('inline=1'))).toBe(true);
});

// The device folds each media status line into its item: the row's st says what the
// card holds. A saved file offers View or Save and no status words; anything else says
// in plain words where the file is, and offers nothing to open until it is saved.
test('status: each item says whether its file is on the card', async ({ page }) => {
  await seedToken(page);
  const rows = [
    { v: 1, dir: 'in', u: 2001, chat: CHAT, date: 1792990800, kind: 'photo', raw: '2001.json', file: '2001.jpg', st: 'ok' },
    { v: 1, dir: 'in', u: 2002, chat: CHAT, date: 1792990810, kind: 'photo', raw: '2002.json', file: '2002.turn.jpg',
      st: 'ok', copy: 'turn', full: 'pending' },
    { v: 1, dir: 'in', u: 2003, chat: CHAT, date: 1792990820, kind: 'voice', raw: '2003.json', file: '2003.ogg', st: 'pending' },
    { v: 1, dir: 'in', u: 2004, chat: CHAT, date: 1792990830, kind: 'document', raw: '2004.json', file: '2004.pdf', st: 'failed' },
    { v: 1, dir: 'in', u: 2005, chat: CHAT, date: 1792990840, kind: 'photo', raw: '2005.json', file: '2005.jpg', st: 'incomplete' },
    { v: 1, dir: 'in', u: 2006, chat: CHAT, date: 1792990850, kind: 'photo', raw: '2006.json', file: '2006.jpg', st: 'missing' },
    { v: 1, dir: 'in', u: 2007, chat: CHAT, date: 1792990860, kind: 'video', raw: '2007.json', st: 'not archived: too large' },
    { v: 1, dir: 'in', u: 2008, chat: CHAT, date: 1792990870, kind: 'text', raw: '2008.json', st: 'ok' },
  ];
  await page.route('**/api/tg/archive**', (r) => {
    const u = new URL(r.request().url());
    const chat = u.searchParams.get('chat'), day = u.searchParams.get('day');
    let body;
    if (!chat) body = { stats: {}, chats: [{ chat: CHAT, days: 1, newest: DAY }] };
    else if (!day) body = { chat, days: [DAY], from: 0, next: 1, more: false };
    else body = { chat, day, rows, from: 0, next: rows.length, more: false };
    return r.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(body) });
  });
  await openArchive(page);
  const row = (i) => page.locator('.tgArcRow').nth(i);
  await expect(page.locator('.tgArcRow')).toHaveCount(rows.length);
  await expect(row(0).locator('.tgArcState')).toHaveCount(0);
  await expect(row(0).locator('button', { hasText: 'View' })).toHaveCount(1);
  await expect(row(1)).toContainText('Smaller copy saved; full size not saved yet');
  await expect(row(1).locator('button', { hasText: 'View' })).toHaveCount(1);
  await expect(row(2)).toContainText('Not saved yet');
  await expect(row(2).locator('button', { hasText: 'Save' })).toHaveCount(0);
  await expect(row(3)).toContainText('Not saved');
  await expect(row(4)).toContainText('Not saved: the memory card refused it');
  await expect(row(5)).toContainText('Not on the memory card');
  await expect(row(5).locator('button', { hasText: 'View' })).toHaveCount(0);
  await expect(row(6)).toContainText('Not saved: too large');   // never fetched, and says why
  await expect(row(7).locator('.tgArcState')).toHaveCount(0);    // a text: nothing to save
});

test('export: Export Day downloads that day as a ZIP', async ({ page }) => {
  await seedToken(page);
  const seen = [];
  await routeArchive(page, seen);
  await openArchive(page);
  const dl = page.waitForEvent('download');
  await page.locator('#tgArcExport').click();
  const file = await dl;
  expect(file.suggestedFilename()).toBe(`${CHAT}-${DAY}.zip`);
  expect(seen).toContain(`/api/tg/archive/zip?chat=${CHAT}&day=${DAY}`);
});

test('an empty archive says so plainly', async ({ page }) => {
  await seedToken(page);
  await page.route('**/api/tg/archive**', (r) => r.fulfill({ status: 200, contentType: 'application/json',
    body: JSON.stringify({ stats: {}, chats: [] }) }));
  await openApp(page);
  await page.locator('.tab[data-p=memory]').click();
  await page.locator('#tgArcGroup summary').click();
  await expect(page.locator('#tgArcStat')).toContainText('Nothing archived yet');
});
