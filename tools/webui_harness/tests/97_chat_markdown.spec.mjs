// CUM-995 (lane MD51): assistant chat bubbles render a safe Markdown subset
// (bold, italic, inline code, fenced blocks, bullet + numbered lists, links,
// line breaks). The model's text is untrusted: the renderer only ever creates
// text nodes / element properties, never innerHTML, so no reply can inject
// markup. User bubbles stay plain text. The pending-reply path (POST /api/chat
// -> GET ?turn=) renders Markdown too.
//
// Golden render: the list-heavy reply below renders to golden/chat_markdown_list.html
// (bless with GOLDEN_UPDATE=1, then LOOK at the screenshot before trusting it).
// Evidence screenshots (desktop + phone, light + dark) go to SHOTS_DIR.
import { test, expect } from '@playwright/test';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { seedToken, openApp } from './_helpers.mjs';

const HERE = path.dirname(fileURLToPath(import.meta.url));

// A list-heavy reply in the shape the real corpus produces (lanes/FOOD
// replies.jsonl 2026-10-24T16:41-41 / 2026-10-30T19:30-120): bold section
// headers, bullet lists, a numbered list, an inline code path, a link, an
// italic phrase and a fenced block.
export const LIST_REPLY =
  "Here's the plan, filling the gap you quoted:\n\n" +
  '**Fixed (your amounts):**\n' +
  '- Beef fillet 100g: ~190 kcal / 28g protein\n' +
  '- Skyr 5% 200g: ~220 kcal / 18g protein\n' +
  '- Berries 150g: ~80 kcal / 1.5g protein\n\n' +
  '**Add to make it a full plate:**\n' +
  '- Salad, big bowl ~150g: ~30 kcal\n' +
  '- Roasted vegetables ~150g: ~150 kcal\n\n' +
  'Subtotal: ~490 kcal / 47.5g protein.\n\n' +
  '**Steps:**\n' +
  '1. Grill the beef 3 min a side\n' +
  '2. Warm the rice\n\n' +
  'Saved in `nutrition/meal_log.csv`.\nSee [the docs](https://docs.cumulo-nimbus.ai/) for the label rules. ' +
  'The last check-in covered *speculative decoding*.\n\n' +
  '```json\n{"kcal": 490}\n```';

// An attacker/model-controlled reply: markup must never become elements.
const XSS_REPLY =
  '<img src=x onerror="window.__xssFired=1"> **bold** [x](javascript:alert(1)) ' +
  '`code` <script>alert(2)</script> [ok](https://example.com/a)';

// Newest-first, as the episodic store returns it (loadChatHistory reverses).
const EPISODIC = {
  messages: [
    { id: 'm3', session: 'web', channel: 'web', role: 'assistant', kind: 'message', text: XSS_REPLY, tags: 'turn:m2' },
    { id: 'm2', session: 'web', channel: 'web', role: 'assistant', kind: 'message', text: LIST_REPLY, tags: 'turn:m1' },
    { id: 'm1', session: 'web', channel: 'web', role: 'user', kind: 'message', text: 'plan my meals **please**', tags: '' },
  ],
};

async function openChatWith(page, episodic) {
  await page.route('**/api/mem/episodic**', (r) =>
    r.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(episodic) }));
  await seedToken(page);
  await openApp(page);
  await page.locator('.tab[data-p=chat]').click();
  await expect(page.locator('#pane-chat')).toBeVisible();
  await expect(page.locator('#chatLog')).toContainText('Subtotal: ~490 kcal');
}

// The assistant bubble that carries the list-heavy reply.
function listBubble(page) {
  return page.locator('#chatLog > div', { hasText: 'Subtotal: ~490 kcal' }).first();
}

test('list-heavy reply renders the markdown subset as real elements', async ({ page }) => {
  await openChatWith(page, EPISODIC);
  const bub = listBubble(page);
  // bold, italic, inline code, link, fenced block
  await expect(bub.locator('b', { hasText: 'Fixed (your amounts):' })).toHaveCount(1);
  await expect(bub.locator('b', { hasText: '1,547' })).toHaveCount(0);   // not in this reply
  await expect(bub.locator('i', { hasText: 'speculative decoding' })).toHaveCount(1);
  await expect(bub.locator('code', { hasText: 'nutrition/meal_log.csv' })).toHaveCount(1);
  const link = bub.locator('a', { hasText: 'the docs' });
  await expect(link).toHaveCount(1);
  await expect(link).toHaveAttribute('href', 'https://docs.cumulo-nimbus.ai/');
  await expect(link).toHaveAttribute('rel', 'noopener noreferrer');
  await expect(bub.locator('pre', { hasText: '{"kcal": 490}' })).toHaveCount(1);
  // bullet list: two uls, three + two items
  await expect(bub.locator('ul')).toHaveCount(2);
  await expect(bub.locator('ul').first().locator('li')).toHaveCount(3);
  await expect(bub.locator('ul').nth(1).locator('li')).toHaveCount(2);
  await expect(bub.locator('ul li', { hasText: 'Beef fillet 100g' })).toHaveCount(1);
  // numbered list
  await expect(bub.locator('ol li', { hasText: 'Grill the beef' })).toHaveCount(1);
  await expect(bub.locator('ol li')).toHaveCount(2);
  // single line breaks inside a paragraph become <br>
  await expect(bub.locator('br')).not.toHaveCount(0);
  // no raw markdown markers survive as text in the rendered bubble
  const txt = await bub.textContent();
  expect(txt).not.toContain('**');
  expect(txt).not.toContain('```');
});

test('XSS payload in an assistant reply never becomes an element', async ({ page }) => {
  await openChatWith(page, EPISODIC);
  const log = page.locator('#chatLog');
  // No element was synthesized from the payload anywhere in the chat.
  await expect(log.locator('img')).toHaveCount(0);
  await expect(log.locator('script')).toHaveCount(0);
  // The payload shows as literal text.
  await expect(log).toContainText('<img src=x onerror');
  await expect(log).toContainText('<script>alert(2)</script>');
  // The onerror never ran.
  expect(await page.evaluate(() => window.__xssFired)).toBeUndefined();
  // The javascript: link did not convert; the https one did.
  await expect(log.locator('a', { hasText: 'x' })).toHaveCount(0);
  await expect(log).toContainText('[x](javascript:alert(1))');
  await expect(log.locator('a', { hasText: 'ok' })).toHaveAttribute('href', 'https://example.com/a');
  // Bold in the same reply still converted.
  await expect(log.locator('b', { hasText: 'bold' })).toHaveCount(1);
});

test('user bubbles stay plain text (markdown markers literal)', async ({ page }) => {
  await openChatWith(page, EPISODIC);
  const userBub = page.locator('#chatLog > div', { hasText: 'plan my meals' }).first();
  await expect(userBub.locator('b')).toHaveCount(0);
  await expect(userBub).toContainText('**please**');
});

test('golden render of the list-heavy reply matches the blessed file', async ({ page }) => {
  await openChatWith(page, EPISODIC);
  const bub = listBubble(page);
  // The bubble's text container is the div the renderer filled.
  const html = await bub.evaluate((el) => el._tx ? el._tx.innerHTML : el.lastChild.innerHTML);
  const goldenPath = path.join(HERE, '..', 'golden', 'chat_markdown_list.html');
  if (process.env.GOLDEN_UPDATE === '1') {
    fs.mkdirSync(path.dirname(goldenPath), { recursive: true });
    fs.writeFileSync(goldenPath, html);
    console.log('blessed golden/chat_markdown_list.html');
  }
  const golden = fs.readFileSync(goldenPath, 'utf8');
  expect(html).toBe(golden);
});

test('a sent turn renders its reply as markdown in its own bubble', async ({ page }) => {
  await page.route('**/api/mem/episodic**', (r) => r.fulfill({ contentType: 'application/json', body: '{"messages":[]}' }));
  await page.route('**/api/chat**', async (route) => {
    const req = route.request();
    const json = (b) => route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(b) });
    if (req.method() === 'POST') return json({ pending: true, turn: 7 });
    const u = new URL(req.url());
    if (u.searchParams.get('watch')) return json({ async: 0, busy: 0 });
    if (u.searchParams.get('turn') === '7') return json({ reply: 'Done:\n\n- **one**\n- two\n\n`ok`', pending: false });
    return json({ reply: '', pending: true });
  });
  await seedToken(page);
  await openApp(page);
  await page.locator('.tab[data-p=chat]').click();
  await expect(page.locator('#pane-chat')).toBeVisible();
  await page.locator('#chatInput').fill('go');
  await page.locator('#chatSend').click();
  const pend = page.locator('#chatLog > div', { hasText: 'Done:' }).first();
  await expect(pend.locator('b', { hasText: 'one' })).toHaveCount(1);
  await expect(pend.locator('ul li')).toHaveCount(2);
  await expect(pend.locator('code', { hasText: 'ok' })).toHaveCount(1);
});

// Evidence screenshots for the lane PR: the chat pane with the list-heavy
// reply, desktop and phone, light and dark scheme. SHOTS_DIR overrides the
// destination (the lane run points it at lanes/MD51/evidence/).
test('evidence screenshots: chat markdown, desktop+phone, light+dark', async ({ page }, testInfo) => {
  await openChatWith(page, EPISODIC);
  const dir = process.env.SHOTS_DIR || path.join('screenshots', 'md51');
  fs.mkdirSync(dir, { recursive: true });
  for (const scheme of ['dark', 'light']) {
    await page.emulateMedia({ colorScheme: scheme });
    await page.waitForTimeout(150);
    await page.screenshot({ path: path.join(dir, `chat-markdown-${testInfo.project.name}-${scheme}.png`), fullPage: true });
  }
});
