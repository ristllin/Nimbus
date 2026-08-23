// CUM-57: chat upgrades - file picker + drag-and-drop upload into chat, with a
// progress/result feedback state, reusing /api/files/upload.
import { test, expect } from '@playwright/test';
import { seedToken, openApp } from './_helpers.mjs';

async function openChat(page) {
  await openApp(page);
  await page.locator('.tab[data-p=chat]').click();
  await expect(page.locator('#pane-chat')).toBeVisible();
}

test('chat pane offers attach + lists supported formats', async ({ page }) => {
  await seedToken(page);
  await openChat(page);
  await expect(page.locator('#chatAttach')).toBeVisible();
  await expect(page.locator('#pane-chat')).toContainText('Supported');
});

test('attaching a file uploads it and announces it in the chat log', async ({ page }) => {
  await seedToken(page);
  await openChat(page);
  await page.locator('#chatFile').setInputFiles({ name: 'report.txt', mimeType: 'text/plain', buffer: Buffer.from('data') });
  await expect(page.locator('#chatUpMsg')).toHaveAttribute('data-fb', 'ok');
  await expect(page.locator('#chatUpMsg')).toContainText('Attached report.txt');
  await expect(page.locator('#chatLog')).toContainText('report.txt');
});

test('a failed upload names the next step and does not break chat', async ({ page }) => {
  await seedToken(page);
  await page.route('**/api/files/upload**', (route) => route.fulfill({ status: 500, body: 'x' }));
  await openChat(page);
  await page.locator('#chatFile').setInputFiles({ name: 'bad.bin', mimeType: 'application/octet-stream', buffer: Buffer.from('x') });
  await expect(page.locator('#chatUpMsg')).toHaveAttribute('data-fb', 'error');
  await expect(page.locator('#chatUpMsg')).toContainText('try again');
});

test('dragging a file over the chat highlights the drop zone', async ({ page }) => {
  await seedToken(page);
  await openChat(page);
  const dt = await page.evaluateHandle(() => new DataTransfer());
  await page.locator('#chatDrop').dispatchEvent('dragover', { dataTransfer: dt });
  await expect(page.locator('#chatDrop')).toHaveClass(/dropping/);
});

test('dropping a file uploads it', async ({ page }) => {
  await seedToken(page);
  await openChat(page);
  // Build a real drop with a DataTransfer carrying a file.
  const dt = await page.evaluateHandle(() => {
    const d = new DataTransfer();
    d.items.add(new File(['hi'], 'dropped.md', { type: 'text/markdown' }));
    return d;
  });
  await page.locator('#chatDrop').dispatchEvent('drop', { dataTransfer: dt });
  await expect(page.locator('#chatUpMsg')).toHaveAttribute('data-fb', 'ok');
  await expect(page.locator('#chatLog')).toContainText('dropped.md');
});
