// Per-model prices and usage (owner 2026-10-13: "different models cost differently
// for the same provider ... graphs should support the different model usages").
//
// The Usage pane reads GET /api/usage/models (fixtures.mjs USAGE_MODELS, produced by the
// real lib/core ledger) and GET /api/usage/history (day, provider, model buckets):
//   - 'This period' lists every model under its provider, and the rows add up to the
//     provider line and the tile (the server's own breakdown, never a client guess);
//   - the chart groups By model (default, remembered) or By provider, draws the top 8
//     series and folds the rest into "Other models", and a legend chip toggles a series;
//   - a history bucket whose model has no row costs the fold price (provRate oIn/oOut),
//     only earlier usage (no model) the residual price;
//   - the Prices table lists each model the owner can price plus "Default for <provider>
//     models" (Tavily: searches only), posts mrate with a model id holding '/' or ':'
//     intact, keeps a price typed in another row across a save, and shows a 400 or 507
//     sentence in #ratemsg;
//   - a Budgets row with a $ cap says when nothing prices the provider's spend;
//   - nothing scrolls sideways at 375 px.
// Set SHOTS_DIR to archive the review screenshots (light and dark, desktop and 375 px).
import { test, expect } from '@playwright/test';
import fs from 'node:fs';
import path from 'node:path';
import { seedToken, openApp } from './_helpers.mjs';

async function openUsage(page) {
  await seedToken(page);
  await openApp(page);
  await page.locator('.tab[data-p=assistant]').click();
  await page.locator('.subtab[data-sp=usage]').click();
  await expect(page.locator('#subpane-usage')).toBeVisible();
  await expect(page.locator('#usageByModel .bym')).toBeVisible();
}

const dollars = (s) => parseFloat(String(s).replace(/[^0-9.]/g, '')) || 0;

test('This period lists each model under its provider and adds up to the tile', async ({ page }) => {
  await openUsage(page);
  const list = page.locator('#usageByModel');
  await expect(list).toContainText('This period by model');
  for (const t of ['OpenAI', 'Anthropic', 'Cumulo Nimbus', 'gpt-5.6-luna', 'anthropic/claude-opus-5',
                   'claude-sonnet-5', 'Earlier usage, model not recorded', 'Searches']) {
    await expect(list).toContainText(t);
  }
  // Each provider line equals the sum of its rows (cents from the server's breakdown).
  const sums = await page.evaluate(() => {
    const out = [];
    let cur = null;
    document.querySelectorAll('#usageByModel .bp, #usageByModel .br').forEach((el) => {
      const v = el.querySelector('.bv').textContent.split('·')[1];
      const c = Math.round(parseFloat(v.replace(/[^0-9.]/g, '')) * 1000);
      if (el.classList.contains('bp')) { cur = { line: c, rows: 0 }; out.push(cur); } else cur.rows += c;
    });
    return out;
  });
  expect(sums.length).toBeGreaterThan(3);
  for (const s of sums) expect(Math.abs(s.line - s.rows)).toBeLessThanOrEqual(1 * sums.length);
  // The 'This period' tile is the sum of the providers' server estimates; 'All time'
  // the sum of the server's all-time estimates (allEst).
  const tile = await page.locator('#usageSummary .tile').nth(1).locator('.cx').textContent();
  expect(dollars(tile.split(' est')[0])).toBeCloseTo((1044 + 722 + 522 + 71 + 29) / 100, 2);
  await expect(page.locator('#usageSummary .tile').nth(1)).toContainText('~');
  const all = await page.locator('#usageSummary .tile').nth(0).locator('.cx').textContent();
  expect(dollars(all.split(' est')[0])).toBeCloseTo((522 + 1095 + 1250 + 71 + 96) / 100, 2);
});

// Gate G4: a history bucket whose model has no row (a 9th model folded into ~other, or
// "~other" itself with no ledger row) is priced at the provider's fold price (provRate
// oIn/oOut, the server's own ~other price), never the cheaper residual price that only
// earlier usage (no model) gets.
test('history $: a model with no row costs the fold price, earlier usage the residual', async ({ page }) => {
  const today = Math.floor(Date.now() / 86400000);
  let days = [{ d: today, prov: 'openai', in: 1000000, out: 0, calls: 0, m: 'gpt-x-unlisted' }];
  await page.route('**/api/usage/history', (r) => r.fulfill({ contentType: 'application/json',
    body: JSON.stringify({ today, days }) }));
  await openUsage(page);
  const week = page.locator('#usageSummary .tile').nth(3);   // Last 7 days
  // openai's fold price is its highest known price: ~$10/M in (a built-in estimate).
  await expect(week).toContainText('$10.00~ est');
  days = [{ d: today, prov: 'openai', in: 1000000, out: 0, calls: 0, m: '~other' }];
  await page.evaluate(() => loadUsageHistory());
  await expect(week).toContainText('$10.00~ est');
  // Earlier usage (no model): the residual price, the owner's $0.20/M.
  days = [{ d: today, prov: 'openai', in: 1000000, out: 0, calls: 0 }];
  await page.evaluate(() => loadUsageHistory());
  await expect(week).toContainText('$0.20 est');
});

test('chart: By model by default, By provider remembered, top 8 + Other models, chips toggle', async ({ page }) => {
  await openUsage(page);
  const legend = page.locator('#usageLegend');
  await expect(page.locator('#groupModel')).toHaveAttribute('aria-pressed', 'true');
  await expect(legend.locator('.lchip')).toHaveCount(9);          // 8 drawn + the fold
  await expect(legend).toContainText('Other models');
  await expect(legend).toContainText('openai/gpt-5.6 (Cumulo)');
  await expect(legend).toContainText('gpt-5.6-luna (OpenAI)');
  // A legend chip hides and re-shows its series.
  const chip = legend.locator('.lchip').first();
  await chip.click();
  await expect(legend.locator('.lchip').first()).toHaveClass(/off/);
  await legend.locator('.lchip').first().click();
  await expect(legend.locator('.lchip').first()).not.toHaveClass(/off/);
  // By provider: one series per provider (tokens: the four LLM providers), and the
  // choice survives a reload.
  await page.locator('#groupProv').click();
  await expect(legend.locator('.lchip')).toHaveCount(4);
  await expect(legend).not.toContainText('Other models');
  await page.reload();
  await page.locator('.tab[data-p=assistant]').click();
  await page.locator('.subtab[data-sp=usage]').click();
  await expect(page.locator('#groupProv')).toHaveAttribute('aria-pressed', 'true');
  await expect(page.locator('#usageLegend .lchip')).toHaveCount(4);
  // The $ unit draws from the server prices (searches cost money too) and marks estimates.
  await page.locator('#unitUsd').click();
  await expect(page.locator('#usageChart')).toBeVisible();
  await expect(page.locator('#usageLegend .lchip')).toHaveCount(5);
  await expect(page.locator('#usageLegend')).toContainText('Tavily');
  await expect(page.locator('#usageChartMsg')).toContainText('~ marks a built-in price estimate');
});

test('chart: before-model-tracking buckets get their own label', async ({ page }) => {
  await page.route('**/api/usage/history', (r) => r.fulfill({ contentType: 'application/json',
    body: JSON.stringify({ today: 20700, days: [
      { d: 20700, prov: 'openai', in: 900, out: 100, calls: 0 },
      { d: 20700, prov: 'openai', in: 500, out: 50, calls: 0, m: '~other' },
      { d: 20699, prov: 'anthropic', in: 800, out: 90, calls: 0, m: 'claude-opus-5', cr: 300, cw: 40 },
    ] }) }));
  await openUsage(page);
  await page.locator('#groupModel').click();
  const legend = page.locator('#usageLegend');
  await expect(legend).toContainText('OpenAI (before model tracking)');
  await expect(legend).toContainText('OpenAI other models');
  await expect(legend).toContainText('claude-opus-5 (Anthropic)');
});

test('Prices: one row per model + the provider default; Tavily shows searches only', async ({ page }) => {
  await openUsage(page);
  const sel = page.locator('#price_prov');
  // The provider select is the registry plus Custom and Tavily (no hard-coded four).
  for (const v of ['cumulo', 'openai', 'anthropic', 'mistral', 'zai', 'custom', 'tavily'])
    await expect(sel.locator(`option[value="${v}"]`)).toHaveCount(1);
  await sel.selectOption('openai');
  const rows = page.locator('#priceRows');
  for (const m of ['gpt-6-astra', 'gpt-5.6', 'gpt-5.6-terra', 'gpt-5.6-luna', 'Default for OpenAI models'])
    await expect(rows).toContainText(m);
  await expect(rows).toContainText('yours');
  await expect(rows).toContainText('~estimate');
  await expect(rows.locator('input[data-f=in][data-m="gpt-5.6-luna"]')).toHaveValue('0.2');
  await expect(rows.locator('input[data-f=in][data-m="gpt-5.6"]')).toHaveAttribute('placeholder', '~5');
  // "Default for OpenAI models" shows the provider default (here the built-in one),
  // not the residual price of old spend.
  await expect(rows.locator('input[data-f=in][data-m="*"]')).toHaveAttribute('placeholder', '~2.5');
  await expect(rows.locator('input[data-f=out][data-m="*"]')).toHaveAttribute('placeholder', '~10');
  // The 5 s poll re-renders the pane: a typed, unsaved price survives it.
  await rows.locator('input[data-f=in][data-m="gpt-5.6-terra"]').fill('1.75');
  await page.locator('#price_prov').focus();
  await page.evaluate(() => renderUsageAll());
  await expect(rows.locator('input[data-f=in][data-m="gpt-5.6-terra"]')).toHaveValue('1.75');
  await sel.selectOption('cumulo');
  await expect(rows).toContainText('Cumulo price');
  await expect(rows).toContainText('anthropic/claude-opus-5');
  await sel.selectOption('anthropic');
  await expect(rows).toContainText('your default');
  await sel.selectOption('tavily');
  await expect(rows).toContainText('Searches');
  await expect(rows.locator('input[data-f=out]')).toHaveCount(0);
  await expect(rows).not.toContainText('Default for');
});

test('Prices: mrate posts micro-USD with a "/" or ":" model id intact', async ({ page }) => {
  const posts = [];
  await page.route('**/api/orch', async (route) => {
    if (route.request().method() === 'POST') {
      posts.push(new URLSearchParams(route.request().postData() || ''));
      return route.fulfill({ status: 200, contentType: 'application/json', body: '{"ok":true}' });
    }
    return route.continue();
  });
  await openUsage(page);
  await page.locator('#price_prov').selectOption('cumulo');
  const rows = page.locator('#priceRows');
  await rows.locator('input[data-f=in][data-m="anthropic/claude-opus-5"]').fill('4.5');
  await rows.locator('input[data-f=out][data-m="anthropic/claude-opus-5"]').fill('22.5');
  await rows.locator('button[data-a=save][data-m="anthropic/claude-opus-5"]').click();
  await expect(page.locator('#ratemsg')).toContainText('Price saved');
  expect(posts.at(-1).get('mrate')).toBe('cumulo:4500000:22500000:anthropic/claude-opus-5');
  await page.locator('#price_prov').selectOption('custom');
  await rows.locator('input[data-f=in][data-m="llama3.1:8b"]').fill('0.1');
  await rows.locator('button[data-a=save][data-m="llama3.1:8b"]').click();
  await expect.poll(() => posts.length).toBe(2);
  expect(posts.at(-1).get('mrate')).toBe('custom:100000:0:llama3.1:8b');
  // Clear posts 0:0; the default row posts the provider rates in cents.
  await page.locator('#price_prov').selectOption('openai');
  await rows.locator('button[data-a=clear][data-m="gpt-5.6-luna"]').click();
  await expect.poll(() => posts.length).toBe(3);
  expect(posts.at(-1).get('mrate')).toBe('openai:0:0:gpt-5.6-luna');
  await expect(page.locator('#ratemsg')).toContainText('Price cleared');
  await rows.locator('input[data-f=in][data-m="*"]').fill('2.5');
  await rows.locator('input[data-f=out][data-m="*"]').fill('10');
  await rows.locator('button[data-a=save][data-m="*"]').click();
  await expect.poll(() => posts.length).toBe(4);
  expect(posts.at(-1).get('rates')).toBe('openai:250:1000:0');
});

test('Prices: saving one row keeps a price typed in another', async ({ page }) => {
  await page.route('**/api/orch', async (route) => {
    if (route.request().method() !== 'POST') return route.continue();
    return route.fulfill({ status: 200, contentType: 'application/json', body: '{"ok":true}' });
  });
  await openUsage(page);
  await page.locator('#price_prov').selectOption('openai');
  const rows = page.locator('#priceRows');
  await rows.locator('input[data-f=in][data-m="gpt-5.6-terra"]').fill('1.75');   // not saved
  await rows.locator('input[data-f=in][data-m="gpt-5.6-luna"]').fill('0.25');
  const reload = page.waitForResponse((r) => r.url().includes('/api/usage/models'));
  await rows.locator('button[data-a=save][data-m="gpt-5.6-luna"]').click();
  await expect(page.locator('#ratemsg')).toContainText('Price saved');
  await reload;
  await page.evaluate(() => renderUsageAll());   // and the next poll
  await expect(rows.locator('input[data-f=in][data-m="gpt-5.6-terra"]')).toHaveValue('1.75');
});

test('Budgets: a $ cap says when nothing prices the spend', async ({ page }) => {
  await openUsage(page);
  const rows = page.locator('#budgetRows');
  const row = (unpriced) => ({ prov: 'openai', tokens: 5000, calls: 0, tokenLimit: 0, callLimit: 0,
    resetDay: 1, over: false, tokIn: 5000, tokOut: 0, centsLimit: 500, estCents: 0,
    approx: false, unpriced, tags: [] });
  // Priced elsewhere (an owner price on another model) but this spend is not: say so.
  await page.evaluate((r) => renderBudgets([r]), row(true));
  await expect(rows).toContainText('Nothing prices this provider yet');
  await page.evaluate((r) => renderBudgets([r]), row(false));
  await expect(rows).not.toContainText('Nothing prices this provider yet');
});

test('Prices: a 400 refusal and a 507 storage-full show their sentence', async ({ page }) => {
  let status = 400;
  await page.route('**/api/orch', async (route) => {
    if (route.request().method() !== 'POST') return route.continue();
    const body = status === 400
      ? { error: 'Price list full (32 models) - clear one first' }
      : { error: 'storage_full', message: "Couldn't save: device storage is full. Remove an unused provider key or connector and try again." };
    return route.fulfill({ status, contentType: 'application/json', body: JSON.stringify(body) });
  });
  await openUsage(page);
  await page.locator('#price_prov').selectOption('openai');
  const rows = page.locator('#priceRows');
  await rows.locator('input[data-f=in][data-m="gpt-5.6-terra"]').fill('2');
  await rows.locator('button[data-a=save][data-m="gpt-5.6-terra"]').click();
  await expect(page.locator('#ratemsg')).toHaveText('Price list full (32 models) - clear one first');
  status = 507;
  await rows.locator('button[data-a=save][data-m="gpt-5.6-terra"]').click();
  await expect(page.locator('#ratemsg')).toContainText('device storage is full');
});

test('Budgets: the D1 hint says a $ cap counts estimates, once', async ({ page }) => {
  await openUsage(page);
  const rows = page.locator('#budgetRows');
  await expect(rows).toContainText('This cap now counts built-in estimates');
  await rows.locator('.d1ok').click();
  await expect(rows).not.toContainText('This cap now counts built-in estimates');
  // The budget provider select is the registry list too.
  await expect(page.locator('#bud_prov option[value=cumulo]')).toHaveCount(1);
  await expect(page.locator('#bud_prov option[value=zai]')).toHaveCount(1);
});

test('no horizontal scroll at 375 px', async ({ page }) => {
  await page.setViewportSize({ width: 375, height: 812 });
  await openUsage(page);
  for (const v of ['openai', 'cumulo', 'tavily']) {
    await page.locator('#price_prov').selectOption(v);
    const over = await page.evaluate(() => document.documentElement.scrollWidth - window.innerWidth);
    expect(over, `sideways scroll with ${v} prices`).toBeLessThanOrEqual(0);
  }
  await page.locator('#groupModel').click();
  const over = await page.evaluate(() => document.documentElement.scrollWidth - window.innerWidth);
  expect(over).toBeLessThanOrEqual(0);
});

// Review screenshots for the owner's eyes (light and dark scheme, desktop and 375 px).
test('review screenshots', async ({ page }, info) => {
  test.skip(info.project.name !== 'desktop', 'captured once, from the desktop project');
  const dir = process.env.SHOTS_DIR || path.join('screenshots', 'usage-models');
  fs.mkdirSync(dir, { recursive: true });
  const shoot = async (loc, name) => {
    await page.mouse.move(0, 0);   // no hover state (a number input's spinner) in the capture
    await loc.scrollIntoViewIfNeeded();
    await page.waitForTimeout(150);
    await loc.screenshot({ path: path.join(dir, name + '.png') });
  };
  for (const scheme of ['dark', 'light']) {
    for (const [w, h, tag] of [[1280, 900, 'desktop'], [375, 812, '375px']]) {
      await page.emulateMedia({ colorScheme: scheme });
      await page.setViewportSize({ width: w, height: h });
      await openUsage(page);
      // The phone's fixed bottom tab bar would sit over a tall element's last rows
      // in an element capture; the owner scrolls past it, so the capture hides it.
      if (w < 600) await page.addStyleTag({ content: '.side{visibility:hidden!important}' });
      const pre = `usage-${scheme}-${tag}`;
      const chartSec = page.locator('.sec', { has: page.locator('#usageChart') });
      await page.locator('#groupModel').click();
      await page.locator('#unitTok').click();
      await shoot(chartSec, `${pre}-chart-by-model-tokens`);
      await page.locator('#unitUsd').click();
      await shoot(chartSec, `${pre}-chart-by-model-usd`);
      await page.locator('#groupProv').click();
      await shoot(chartSec, `${pre}-chart-by-provider-usd`);
      await page.locator('#unitTok').click();
      await shoot(chartSec, `${pre}-chart-by-provider-tokens`);
      await page.locator('#groupModel').click();
      await shoot(page.locator('.sec', { has: page.locator('#usageByModel') }), `${pre}-this-period-by-model`);
      for (const prov of ['openai', 'anthropic', 'cumulo']) {
        await page.locator('#price_prov').selectOption(prov);
        await shoot(page.locator('#pricesSec'), `${pre}-prices-${prov}`);
      }
      await shoot(page.locator('#budgetsSec'), `${pre}-budgets`);
    }
  }
});
