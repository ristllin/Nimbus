// The monthly spend ceiling (lane DEF, owner requirement 21 Oct 2026): a default of
// 20 USD across all providers, shown and changed on the Usage page, stated by the
// setup wizard, and the model picker offers the stronger models with their price.
//   - Usage > Budgets shows this month's estimate against the ceiling, red with
//     "paid calls paused" while frozen, and the input starts at the ceiling;
//   - Save posts spendCeil in CENTS; an amount outside 1..100,000 is refused on the
//     page with a sentence and nothing is posted;
//   - the wizard's provider step and its last step state the defaults and the ceiling;
//   - a model the device prices carries "per 1M" in the picker;
//   - a web chat send while paused shows the device's reason at once;
//   - nothing scrolls sideways at 375 px.
import { test, expect } from '@playwright/test';
import { seedToken, openApp } from './_helpers.mjs';
import { STATE, ORCH, CEILING } from '../fixtures.mjs';

function orchWith(ceiling) {
  return { ...ORCH, usage: { ...ORCH.usage, ceiling } };
}

async function openUsage(page, ceiling = CEILING, posts = null) {
  await page.route('**/api/orch', (route) => {
    if (route.request().method() === 'GET')
      return route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(orchWith(ceiling)) });
    if (posts) posts.push(new URLSearchParams(route.request().postData() || ''));
    return route.fulfill({ status: 200, contentType: 'application/json', body: '{"ok":true}' });
  });
  await seedToken(page);
  await openApp(page);
  await page.locator('.tab[data-p=assistant]').click();
  await page.locator('.subtab[data-sp=usage]').click();
  await expect(page.locator('#subpane-usage')).toBeVisible();
  await expect(page.locator('#ceilMeter')).toContainText('this month');
}

test('the Usage page shows the month against the ceiling', async ({ page }) => {
  await openUsage(page);
  const meter = page.locator('#ceilMeter');
  await expect(meter).toContainText('$6.40~ of $20.00 this month');
  await expect(meter).toContainText('Starts again 1 November');
  await expect(meter).not.toContainText('paused');
  await expect(page.locator('#ceil_usd')).toHaveValue('20');
  await expect(page.locator('#ceilmsg')).toContainText('All providers together');
});

test('while frozen the meter is full, red and says paid calls are paused', async ({ page }) => {
  await openUsage(page, { ...CEILING, spentCents: 2013, frozen: true });
  const meter = page.locator('#ceilMeter');
  await expect(meter).toContainText('paid calls paused');
  const bar = meter.locator('.g i');
  await expect(bar).toHaveAttribute('style', /width:100%/);
  await expect(bar).toHaveAttribute('style', /#f0687a/);
});

test('Save posts the ceiling in cents; an amount out of range posts nothing', async ({ page }) => {
  const posts = [];
  await openUsage(page, CEILING, posts);
  await page.locator('#ceil_usd').fill('0');
  await page.locator('#ceilsave').click();
  await expect(page.locator('#ceilmsg')).toContainText('from $1 to $100,000');
  expect(posts.length).toBe(0);
  await page.locator('#ceil_usd').fill('50');
  await page.locator('#ceilsave').click();
  await expect(page.locator('#ceilmsg')).toContainText('Monthly spend ceiling set to $50');
  expect(posts.length).toBe(1);
  expect(posts[0].get('spendCeil')).toBe('5000');
});

test('the wizard states the default models and the ceiling', async ({ page }) => {
  await seedToken(page);
  await page.route('**/api/state', (route) =>
    route.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify({ ...STATE, needsOnboarding: true }) }));
  await page.goto('/?onboard=provider');
  await expect(page.locator('#onb_prov')).toBeVisible();
  await expect(page.locator('#onb_defaults')).toContainText('standard model for the assistant');
  await expect(page.locator('#onb_defaults')).toContainText('$20.00');
  await expect(page.locator('#onb_defaults')).toContainText('Assistant > Usage');
});

test('the wizard summary names the monthly spend ceiling', async ({ page }) => {
  await seedToken(page);
  await page.route('**/api/state', (route) =>
    route.fulfill({
      status: 200,
      contentType: 'application/json',
      body: JSON.stringify({ ...STATE, needsOnboarding: true, sta: true, provVerified: true }),
    }));
  // The last step (done) by its saved index: only the provider step deep-links.
  await page.addInitScript(() => {
    try { localStorage.setItem('nimbusOnboardStep', '9'); } catch (e) {}
  });
  await page.goto('/');
  await expect(page.locator('#onb_summary')).toContainText('Monthly spend ceiling: $20.00 across all providers');
});

test('the model picker shows a price per million tokens', async ({ page }) => {
  await seedToken(page);
  await openApp(page);
  // The picker prices models from the Usage page's list; wait for it, then for a rebuild.
  await page.locator('.tab[data-p=assistant]').click();
  await page.locator('.subtab[data-sp=llm]').click();
  const group = page.locator('#subpane-llm details.setgroup:has(#provs)');
  if (!(await group.evaluate((d) => d.open))) await group.locator('summary').first().click();
  await expect(page.locator('#provs #prov_anthropic')).toBeVisible();
  await expect
    .poll(async () => (await page.locator('#orchM_anthropic option').allInnerTexts()).join('\n'), { timeout: 15000 })
    .toContain('per 1M');
  const labels = (await page.locator('#orchM_anthropic option').allInnerTexts()).join('\n');
  expect(labels).toMatch(/claude-sonnet-5[^\n]*\$3\/\$15 per 1M/);
});

test('a web chat send while paused shows the reason at once', async ({ page }) => {
  await page.route('**/api/chat', (route) => {
    if (route.request().method() !== 'POST') return route.continue();
    return route.fulfill({
      status: 409,
      contentType: 'application/json',
      body: JSON.stringify({
        error: "Paid AI calls are paused: this month's spend ceiling is reached. Raise it on Assistant > Usage.",
        code: 'spend_ceiling',
      }),
    });
  });
  await seedToken(page);
  await openApp(page);
  await page.locator('.tab[data-p=chat]').click();
  await page.locator('#chatInput').fill('hello');
  await page.locator('#chatInput').press('Enter');
  await expect(page.locator('#chatLog')).toContainText('Paid AI calls are paused', { timeout: 10000 });
});

// Lane DEF2: provider limits are hard stops; with them set the overall ceiling is off by
// default (the owner can also turn it off); the owner's daily warning.
test('with provider limits the overall ceiling reads off and each limit is listed', async ({ page }) => {
  await openUsage(page, {
    ...CEILING, active: false, ownerSet: false, spentCents: 1864,
    providers: [
      { p: 'anthropic', capCents: 5000, spentCents: 5012, frozen: true, resetsOn: '1 November' },
      { p: 'openai', capCents: 1000, spentCents: 600, frozen: false, resetsOn: '1 November' },
      { p: 'mistral', capCents: 0, spentCents: 49, frozen: false, resetsOn: '1 November' },
    ],
  });
  await expect(page.locator('#ceilMeter')).toContainText('no overall ceiling');
  await expect(page.locator('#ceilMeter')).toContainText('Off: your provider limits apply');
  const prov = page.locator('#ceilProv');
  await expect(prov).toContainText('Anthropic: $50.12~ of $50.00');
  await expect(prov).toContainText('paused');
  await expect(prov).toContainText('OpenAI: $6.00~ of $10.00');
  await expect(prov).not.toContainText('Mistral');   // no limit: not listed
  await expect(page.locator('#ceil_on')).not.toBeChecked();
});

test('unchecking the ceiling posts off; the daily warning posts cents', async ({ page }) => {
  const posts = [];
  await openUsage(page, CEILING, posts);
  await page.locator('#ceil_on').uncheck();
  await page.locator('#ceilsave').click();
  await expect(page.locator('#ceilmsg')).toContainText('The overall monthly ceiling is off');
  expect(posts[0].get('spendCeil')).toBe('off');
  await page.locator('#daily_usd').fill('5');
  await page.locator('#dailysave').click();
  await expect(page.locator('#dailymsg')).toContainText('Daily spend warning set to $5.00');
  expect(posts[1].get('dailyWarn')).toBe('500');
  await page.locator('#daily_usd').fill('');
  await page.locator('#dailysave').click();
  await expect(page.locator('#dailymsg')).toContainText('built-in daily warning');
  expect(posts[2].get('dailyWarn')).toBe('0');
  await page.locator('#daily_usd').fill('0.25');   // a sub-dollar warning is kept in cents
  await page.locator('#dailysave').click();
  await expect(page.locator('#dailymsg')).toContainText('Daily spend warning set to $0.25');
  expect(posts[3].get('dailyWarn')).toBe('25');
  await page.locator('#daily_usd').fill('-1');
  await page.locator('#dailysave').click();
  await expect(page.locator('#dailymsg')).toContainText('Enter an amount');
  expect(posts.length).toBe(4);
});

test('a device without the daily warning (the hosted one) does not offer it', async ({ page }) => {
  const { dailyWarnCents, todayCents, ...older } = CEILING;
  await openUsage(page, older);
  await expect(page.locator('#dailyRow')).toBeHidden();
  await expect(page.locator('#ceil_usd')).toBeVisible();
});

test('the ceiling block does not scroll sideways at 375 px', async ({ page }) => {
  await page.setViewportSize({ width: 375, height: 800 });
  await openUsage(page, { ...CEILING, spentCents: 1700 });
  const overflow = await page.evaluate(() => document.documentElement.scrollWidth - window.innerWidth);
  expect(overflow).toBeLessThanOrEqual(0);
  await expect(page.locator('#ceilMeter .g i')).toHaveAttribute('style', /#e0b870/);   // 85%: amber
});
