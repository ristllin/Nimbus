// CUM-25: the five-destination IA + fluid layout.
//  - exactly five destinations: Home, Chat, Memory, Assistant, Device
//  - each shows its expected content; Assistant stacks providers/tools/usage/routines
//  - default is Home; quick actions navigate; reduced-motion respected
//  - responsive: phone shows the bottom tab bar; content column caps at 1280px
import { test, expect } from '@playwright/test';
import { seedToken, collectErrors, openApp } from './_helpers.mjs';

const DESTS = ['home', 'chat', 'memory', 'assistant', 'device'];

test('exactly five destinations with the approved labels', async ({ page }) => {
  await seedToken(page);
  await openApp(page);
  const tabs = page.locator('nav.tabs .tab');
  await expect(tabs).toHaveCount(5);
  await expect(tabs).toHaveText([/Home/, /Chat/, /Memory/, /Assistant/, /Device/]);
  for (const d of DESTS) await expect(page.locator(`.tab[data-p=${d}]`)).toHaveCount(1);
});

test('default destination is Home and shows tiles + active sessions + quick actions', async ({ page }) => {
  await seedToken(page);
  await openApp(page);
  await expect(page.locator('.tab[data-p=home]')).toHaveClass(/on/);
  await expect(page.locator('#pane-dash')).toBeVisible();
  await expect(page.locator('#pane-dash #devtiles')).toBeVisible();
  await expect(page.locator('#pane-dash #dashJobs')).toBeVisible();
  await expect(page.locator('#pane-dash [data-go]').first()).toBeVisible();
  await expect(page.locator('#pane-dash')).toContainText('Quick actions');
});

test('Home populates active sessions from the orchestrator on open', async ({ page }) => {
  await seedToken(page);
  await openApp(page);
  await expect(page.locator('#pane-dash #dashJobs')).toContainText('sess-1');
});

test('each destination reveals its content and hides the others', async ({ page }) => {
  await seedToken(page);
  const { errors } = collectErrors(page);
  await openApp(page);
  await page.locator('.tab[data-p=memory]').click();
  await expect(page.locator('#pane-mem')).toBeVisible();
  await expect(page.locator('#pane-dash')).toBeHidden();

  // Assistant stacks the capabilities, usage, and routines panes together.
  await page.locator('.tab[data-p=assistant]').click();
  await expect(page.locator('#pane-harness')).toBeVisible();
  await expect(page.locator('#pane-usage')).toBeVisible();
  await expect(page.locator('#pane-gov')).toBeVisible();

  await page.locator('.tab[data-p=device]').click();
  await expect(page.locator('#pane-set')).toBeVisible();
  await expect(page.locator('#pane-harness')).toBeHidden();
  expect(errors, errors.join('\n')).toEqual([]);
});

test('quick actions on Home navigate to other destinations', async ({ page }) => {
  await seedToken(page);
  await openApp(page);
  await page.locator('#pane-dash [data-go=assistant]').click();
  await expect(page.locator('.tab[data-p=assistant]')).toHaveClass(/on/);
  await expect(page.locator('#pane-harness')).toBeVisible();
});

test('the old standalone Sessions destination is gone (folded into Home)', async ({ page }) => {
  await seedToken(page);
  await openApp(page);
  await expect(page.locator('.tab[data-p=fleet]')).toHaveCount(0);
  await expect(page.locator('#pane-fleet')).toHaveCount(0);
});

test('content column never scrolls the page body sideways', async ({ page }) => {
  await seedToken(page);
  await openApp(page);
  const overflow = await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth + 1);
  expect(overflow).toBe(true);
});
