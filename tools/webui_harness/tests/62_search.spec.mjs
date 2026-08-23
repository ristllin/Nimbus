// CUM-62: global search - one palette over settings/actions, files, memory,
// sessions, and the embedded docs pack; keyboard reachable; results grouped.
import { test, expect } from '@playwright/test';
import { seedToken, openApp } from './_helpers.mjs';

test('the search index (static) is pure and keyword-matches', async ({ page }) => {
  await seedToken(page);
  await openApp(page);
  const res = await page.evaluate(() => ({
    all: searchIndexStatic('').length,
    update: searchIndexStatic('update').map((x) => x.label),
    cloud: searchIndexStatic('pairing').map((x) => x.label),
    none: searchIndexStatic('zzzznope').length,
  }));
  expect(res.all).toBeGreaterThanOrEqual(10);
  expect(res.update).toContain('Check for updates');
  expect(res.cloud).toContain('Pair with the cloud');
  expect(res.none).toBe(0);
});

test('Ctrl+K opens the palette, Esc closes it', async ({ page }) => {
  await seedToken(page);
  await openApp(page);
  await page.keyboard.press('Control+k');
  await expect(page.locator('#searchOverlay')).toBeVisible();
  await expect(page.locator('#searchInput')).toBeFocused();
  await page.keyboard.press('Escape');
  await expect(page.locator('#searchOverlay')).toHaveCount(0);
});

test('"/" opens the palette when not typing in a field', async ({ page }) => {
  await seedToken(page);
  await openApp(page);
  await page.keyboard.press('/');
  await expect(page.locator('#searchOverlay')).toBeVisible();
});

test('the sidebar Search button opens the palette', async ({ page }) => {
  await seedToken(page);
  await openApp(page);
  await page.locator('#globalSearchBtn').click();
  await expect(page.locator('#searchOverlay')).toBeVisible();
});

test('results are grouped across sources', async ({ page }) => {
  await seedToken(page);
  await openApp(page);
  await page.locator('#globalSearchBtn').click();
  await page.locator('#searchInput').fill('update');
  // Settings & actions is synchronous; docs arrives from /api/docs/search.
  await expect(page.locator('.sgroup', { hasText: 'Settings & actions' })).toBeVisible();
  await expect(page.locator('.sresult', { hasText: 'Check for updates' })).toBeVisible();
  await expect(page.locator('.sgroup', { hasText: 'Docs' })).toBeVisible();
  await expect(page.locator('.sresult', { hasText: 'battery gate' })).toBeVisible();
});

test('memory results appear from the memory source', async ({ page }) => {
  await seedToken(page);
  await openApp(page);
  await page.locator('#globalSearchBtn').click();
  await page.locator('#searchInput').fill('ring');
  await expect(page.locator('.sgroup', { hasText: 'Memory' })).toBeVisible();
  await expect(page.locator('.sresult', { hasText: 'one arc per active session' })).toBeVisible();
});

test('a memory search result opens the Long-term memory section', async ({ page }) => {
  await seedToken(page);
  await openApp(page);
  await page.locator('#globalSearchBtn').click();
  await page.locator('#searchInput').fill('ring');
  await page.locator('.sresult', { hasText: 'one arc per active session' }).click();
  await expect(page.locator('.tab[data-p=memory]')).toHaveClass(/on/);
  // The Long-term memory <details> in #pane-mem must be expanded (its recall
  // search input becomes visible) - proves _openGroup reaches non-Device panes.
  await expect(page.locator('#memq')).toBeVisible();
});

test('Enter on a result navigates to its destination', async ({ page }) => {
  await seedToken(page);
  await openApp(page);
  await page.locator('#globalSearchBtn').click();
  await page.locator('#searchInput').fill('memory');
  // First result is the "Memory" destination; Enter activates it.
  await page.keyboard.press('Enter');
  await expect(page.locator('#searchOverlay')).toHaveCount(0);
  await expect(page.locator('.tab[data-p=memory]')).toHaveClass(/on/);
});
