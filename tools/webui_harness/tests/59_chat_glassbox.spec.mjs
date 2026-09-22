// Glass box (A4): the chat pane folds each turn's tool_output/llm_response rows
// into ONE collapsed <details> anchored to that turn. A turn with no chat bubble
// on the page - a finished-and-reply-less routine/synthesis turn, or an older
// errored turn - must NOT free-float as a context-free "N tool calls" row; only
// the newest owner turn still in flight (user row present, no reply yet) floats.
// Regression guard for the "wall of ~40 tool calls after saying hi" report, where
// the orphan dump stacked every past reply-less turn's trace with no context.
import { test, expect } from '@playwright/test';
import { seedToken, openApp } from './_helpers.mjs';

// Newest-first, exactly as the episodic store returns it (loadChatHistory reverses).
// One answered turn (m10), two reply-less historical orphans (m20, m30), and one
// in-flight owner turn (m40: newest user row, no reply).
const EPISODIC = {
  messages: [
    { id: 'm41', session: 'web', channel: 'web', role: 'tool', kind: 'tool_output', text: 'TOOL live_gamma -> running', tags: 'turn:m40' },
    { id: 'm40', session: 'web', channel: 'web', role: 'user', kind: 'message', text: 'hi', tags: '' },
    { id: 'm31', session: 'web', channel: 'web', role: 'tool', kind: 'tool_output', text: 'TOOL orphan_beta -> ok', tags: 'turn:m30' },
    { id: 'm21', session: 'web', channel: 'web', role: 'tool', kind: 'tool_output', text: 'TOOL orphan_alpha -> ok', tags: 'turn:m20' },
    { id: 'm13', session: 'web', channel: 'web', role: 'system', kind: 'log', text: '{"host":"anthropic","model":"claude-sonnet-5","tools":1,"ok":true,"in":2700,"out":720}', tags: 'ev:turnend,turn:m10' },
    { id: 'm12', session: 'web', channel: 'web', role: 'assistant', kind: 'message', text: 'Here is the answer.', tags: 'turn:m10' },
    { id: 'm11', session: 'web', channel: 'web', role: 'tool', kind: 'tool_output', text: 'TOOL answered_delta -> 3 hits', tags: 'turn:m10' },
    { id: 'm10', session: 'web', channel: 'web', role: 'user', kind: 'message', text: 'first question', tags: '' },
  ],
};

async function openChatWith(page, episodic) {
  await page.route('**/api/mem/episodic**', (r) =>
    r.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(episodic) }));
  await openApp(page);
  await page.locator('.tab[data-p=chat]').click();
  await expect(page.locator('#pane-chat')).toBeVisible();
  // loadChatHistory paints the log; wait for the answered reply to land.
  await expect(page.locator('#chatLog')).toContainText('Here is the answer.');
}

test('answered and in-flight turns render; reply-less historical orphans do not', async ({ page }) => {
  await seedToken(page);
  await openChatWith(page, EPISODIC);

  // Both real turns' bubbles are present.
  await expect(page.locator('#chatLog')).toContainText('first question');
  await expect(page.locator('#chatLog')).toContainText('hi');

  // Exactly two trace disclosures: the answered turn + the one in-flight turn.
  await expect(page.locator('#chatLog details')).toHaveCount(2);

  // The answered turn's own tool row and the in-flight turn's tool row are present.
  await expect(page.locator('#chatLog')).toContainText('answered_delta');
  await expect(page.locator('#chatLog')).toContainText('live_gamma');

  // The context-free historical orphans are dropped, not stacked as bare groups.
  await expect(page.locator('#chatLog')).not.toContainText('orphan_alpha');
  await expect(page.locator('#chatLog')).not.toContainText('orphan_beta');
});

test('no in-flight turn: only anchored turns render, no floating trace', async ({ page }) => {
  await seedToken(page);
  // Drop the in-flight m40 rows; keep the answered turn and the two orphans.
  const noInflight = { messages: EPISODIC.messages.filter((m) => m.id !== 'm40' && m.id !== 'm41') };
  await openChatWith(page, noInflight);

  // Only the answered turn's single disclosure survives; orphans stay dropped.
  await expect(page.locator('#chatLog details')).toHaveCount(1);
  await expect(page.locator('#chatLog')).toContainText('answered_delta');
  await expect(page.locator('#chatLog')).not.toContainText('orphan_alpha');
  await expect(page.locator('#chatLog')).not.toContainText('orphan_beta');
});
