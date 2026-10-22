// The chat history pane is honest about what it has not loaded (19 Oct triage):
// on 19 Oct the pane showed a day with five hours missing, because it read 200 rows
// and ignored indexComplete, olderExists, searchedTo and nextBefore. It now says when
// older history is still being indexed, offers Load Older with the device's own
// cursor (passed back unchanged, never computed here), and says how far back the
// page searched.
import { test, expect } from '@playwright/test';
import { seedToken, openApp } from './_helpers.mjs';

const msg = (id, role, text) => ({ id, session: 'web', kind: 'message', role, text, tags: '' });

const NEWEST = {
  messages: [msg('m000002a1', 'assistant', 'newest reply'), msg('m000002a0', 'user', 'newest question')],
  total: 1234, indexComplete: false, olderExists: true, nextBefore: 'm000001f3-cursor', searchedTo: '2026-10-19',
};
const OLDER = {
  messages: [msg('m000001f2', 'assistant', 'older reply'), msg('m000001f1', 'user', 'older question')],
  total: 1234, indexComplete: false, olderExists: false, nextBefore: '', searchedTo: '2026-10-18',
};

async function routeEpisodic(page, seen) {
  await page.route('**/api/mem/episodic**', (r) => {
    const u = new URL(r.request().url());
    seen.push(u.search);
    const body = u.searchParams.get('before') ? OLDER : NEWEST;
    return r.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(body) });
  });
}

async function openChat(page) {
  await openApp(page);
  await page.locator('.tab[data-p=chat]').click();
  await expect(page.locator('#pane-chat')).toBeVisible();
}

test('an index still building shows the divider with the row count', async ({ page }) => {
  await seedToken(page);
  const seen = [];
  await routeEpisodic(page, seen);
  await openChat(page);
  await expect(page.locator('#chatIndexing')).toHaveText('Older history is still being indexed: 1234 rows so far.');
  await expect(page.locator('#chatSearchedTo')).toHaveText('Searched back to 2026-10-19');
});

test('Load Older asks for exactly the device cursor and shows the older rows first', async ({ page }) => {
  await seedToken(page);
  const seen = [];
  await routeEpisodic(page, seen);
  await openChat(page);
  await expect(page.locator('#chatLog')).toContainText('newest question');
  await page.locator('#chatOlder').click();
  await expect(page.locator('#chatLog')).toContainText('older question');
  const older = seen.find((q) => q.includes('before='));
  expect(new URLSearchParams(older).get('before')).toBe('m000001f3-cursor');
  // Oldest first: the older page renders above the newest one.
  const text = await page.locator('#chatLog').innerText();
  expect(text.indexOf('older question')).toBeLessThan(text.indexOf('newest question'));
  // The oldest page has nothing older: the control is gone.
  await expect(page.locator('#chatOlder')).toHaveCount(0);
});

test('a complete index with nothing older shows neither the divider nor the control', async ({ page }) => {
  await seedToken(page);
  await page.route('**/api/mem/episodic**', (r) => r.fulfill({ status: 200, contentType: 'application/json',
    body: JSON.stringify({ ...NEWEST, indexComplete: true, olderExists: false, nextBefore: '' }) }));
  await openChat(page);
  await expect(page.locator('#chatLog')).toContainText('newest question');
  await expect(page.locator('#chatIndexing')).toHaveCount(0);
  await expect(page.locator('#chatOlder')).toHaveCount(0);
});

// The via chip names each provider a switched turn billed, in both row shapes the
// device writes: [host, model, in, out, ...] and, when it had to shrink the row,
// [host, in, out].
test('the via chip reads both turn-end row shapes', async ({ page }) => {
  await seedToken(page);
  await openApp(page);
  const text = await page.evaluate(() => {
    const out = [];
    for (const via of [[['anthropic', 'claude-haiku-5-5', 1200, 80, 0, 0], ['mistral', 'mistral-small-latest', 900, 40]],
                       [['anthropic', 1200, 80], ['mistral', 900, 40]]]) {
      const b = document.createElement('div');
      _turnChip(b, { text: JSON.stringify({ host: 'mistral', in: 2100, out: 120, via }) });
      out.push(b.textContent);
    }
    return out;
  });
  expect(text[0]).toContain('via anthropic · claude-haiku-5-5 1.2k/80, mistral · mistral-small-latest 900/40');
  expect(text[1]).toContain('via anthropic 1.2k/80, mistral 900/40');
});
