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
