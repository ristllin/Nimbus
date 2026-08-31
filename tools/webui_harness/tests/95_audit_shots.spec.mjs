// CUM-214 render-audit capture: every page / subtab / state at desktop + phone.
// Regenerable QA artifacts (screenshots/ is gitignored). Run just this file:
//   npx playwright test 95_audit_shots
// Output lands in screenshots/audit/<project>-<name>.png. A focused Safety
// subpane shot (safety-focus) is the before/after evidence for the checkbox fix.
import { test, expect } from '@playwright/test';
import { seedToken, openApp, assertPane } from './_helpers.mjs';

const DIR = 'screenshots/audit';

async function shot(page, testInfo, name, locator) {
  const proj = testInfo.project.name; // desktop | phone
  const target = locator || page;
  await target.screenshot({ path: `${DIR}/${proj}-${name}.png`, fullPage: !locator });
}

// Expand every collapsible setgroup + open every tap-? hint so audit shots show
// the real content, not just closed summaries.
async function expandAll(page) {
  await page.evaluate(() => {
    document.querySelectorAll('details').forEach((d) => { d.open = true; });
    document.querySelectorAll('.hint.tip').forEach((h) => h.classList.add('open'));
    document.querySelectorAll('button.qh').forEach((b) => b.setAttribute('aria-expanded', 'true'));
    // Reveal sections the JS hides until a condition (battery, what-next).
    ['battsec', 'whatNext'].forEach((id) => { const e = document.getElementById(id); if (e) e.style.display = ''; });
  });
  await page.waitForTimeout(250);
}

const TOP = ['home', 'chat', 'memory', 'assistant', 'device'];

// A subpane must actually be on screen before its shot is archived - a capture of
// the wrong subpane asserts nothing otherwise (AGENTS.md "capture never inspected").
async function assertSubpane(page, sp) {
  await expect(page.locator(`#subpane-${sp}`), `${sp} subpane did not render`).toBeVisible();
}

// Top-level destinations, default (collapsed) state.
for (const dest of TOP) {
  test(`top ${dest}`, async ({ page }, testInfo) => {
    await seedToken(page);
    await openApp(page);
    await page.locator(`.tab[data-p=${dest}]`).click();
    await assertPane(page, dest);   // right pane, or fail before capturing
    await page.waitForTimeout(500);
    await shot(page, testInfo, `top-${dest}`);
  });
}

// Device + Memory with every setgroup expanded (their real surface area).
for (const dest of ['device', 'memory']) {
  test(`expanded ${dest}`, async ({ page }, testInfo) => {
    await seedToken(page);
    await openApp(page);
    await page.locator(`.tab[data-p=${dest}]`).click();
    await assertPane(page, dest);
    await page.waitForTimeout(400);
    await expandAll(page);
    await shot(page, testInfo, `expanded-${dest}`);
  });
}

// Assistant: each of the seven exclusive subtabs.
const SUBTABS = ['llm', 'connectors', 'tools', 'skills', 'routines', 'usage', 'safety'];
for (const sp of SUBTABS) {
  test(`assistant ${sp}`, async ({ page }, testInfo) => {
    await seedToken(page);
    await openApp(page);
    await page.locator('.tab[data-p=assistant]').click();
    await assertPane(page, 'assistant');
    await page.locator(`.subtab[data-sp=${sp}]`).click();
    await assertSubpane(page, sp);   // the chosen subpane, or fail before capturing
    await page.waitForTimeout(400);
    await expandAll(page);
    await shot(page, testInfo, `assistant-${sp}`);
  });
}

// Focused Safety subpane shot: the checkbox-alignment exemplar evidence.
test('safety focus', async ({ page }, testInfo) => {
  await seedToken(page);
  await openApp(page);
  await page.locator('.tab[data-p=assistant]').click();
  await page.locator('.subtab[data-sp=safety]').click();
  await assertSubpane(page, 'safety');
  await page.waitForTimeout(400);
  await shot(page, testInfo, 'safety-focus', page.locator('#subpane-safety'));
});

// Global search command palette (Ctrl K).
test('search overlay', async ({ page }, testInfo) => {
  await seedToken(page);
  await openApp(page);
  await page.keyboard.press('Control+k');
  await expect(page.locator('#searchOverlay')).toBeVisible();
  await page.locator('#searchInput').fill('battery');
  await expect(page.locator('.sresult').first()).toBeVisible();
  await shot(page, testInfo, 'search-overlay');
});
