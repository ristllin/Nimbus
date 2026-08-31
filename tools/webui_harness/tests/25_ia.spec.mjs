// CUM-25: the five-destination IA + fluid layout.
//  - exactly five destinations: Home, Chat, Memory, Assistant, Device
//  - each shows its expected content; Assistant is one page with seven exclusive subtabs
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

  // Assistant is one page with seven exclusive subtabs; the default is Models
  // (only its subpane shows) and choosing another shows only that subpane.
  await page.locator('.tab[data-p=assistant]').click();
  await expect(page.locator('.subtab[data-sp=llm]')).toHaveClass(/on/);
  await expect(page.locator('#subpane-llm')).toBeVisible();
  await expect(page.locator('#subpane-usage')).toBeHidden();
  await expect(page.locator('#subpane-safety')).toBeHidden();
  await page.locator('.subtab[data-sp=safety]').click();
  await expect(page.locator('#subpane-safety')).toBeVisible();
  await expect(page.locator('#subpane-llm')).toBeHidden();
  await expect(page.locator('#subpane-usage')).toBeHidden();

  await page.locator('.tab[data-p=device]').click();
  await expect(page.locator('#pane-set')).toBeVisible();
  await expect(page.locator('#pane-harness')).toBeHidden();
  expect(errors, errors.join('\n')).toEqual([]);
});

test('quick actions on Home navigate to other destinations', async ({ page }) => {
  await seedToken(page);
  await openApp(page);
  // Home may offer more than one shortcut into Assistant (a quick action plus an
  // alert's jump); any of them must navigate there.
  await page.locator('#pane-dash [data-go=assistant]').first().click();
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
