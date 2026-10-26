// Lane QW4: a custom endpoint on a paid host is counted at the highest built-in price
// until the owner prices it (never at 0, so its spend reaches the monthly ceiling), and
// the Usage card says the figure is an estimate. One on the local network is free and
// gets no such line.
import { test, expect } from '@playwright/test';
import { seedToken, openApp } from './_helpers.mjs';

async function openUsage(page) {
  await seedToken(page);
  await openApp(page);
  await page.locator('.tab[data-p=assistant]').click();
  await page.locator('.subtab[data-sp=usage]').click();
  await expect(page.locator('#usageByModel .bym')).toBeVisible();
}

// The card rendered from a ledger whose custom endpoint row carries price source `src`.
async function renderCustom(page, src) {
  return page.evaluate((s) => {
    const est = s === 'est' ? 19 : 0;
    _usageBP = [{ prov: 'custom', tokIn: 16000, tokOut: 500, calls: 0, estCents: est, approx: s === 'est' }];
    _usageModels = { asOf: '2026-10', residual: [], editable: {}, provRate: { custom: {} },
      models: [{ prov: 'custom', m: 'openrouter/some-model', pIn: 16000, pOut: 500, pCr: 0, pCw: 0,
                 tIn: 16000, tOut: 500, rIn: 10000000, rOut: 50000000, src: s, est }] };
    renderByModel();
    return document.getElementById('usageByModel').textContent;
  }, src);
}

test('a paid custom endpoint estimate says so, a local one does not', async ({ page }) => {
  await openUsage(page);
  const paid = await renderCustom(page, 'est');
  expect(paid).toContain('Custom endpoint: an estimate at the highest built-in price until you set its real price');
  expect(paid).toContain('~estimate');
  expect(paid).toContain('$0.19~');
  const local = await renderCustom(page, 'none');
  expect(local).not.toContain('Custom endpoint: an estimate');
});
