// Screenshot archive (CUM-25 visual confirmation, CUM-74 evidence): capture each
// destination on desktop and phone into screenshots/. Regenerable QA artifacts,
// not committed. Run: npx playwright test 90_screenshots
import { test } from '@playwright/test';
import { seedToken, openApp, assertPane } from './_helpers.mjs';

const DESTS = ['home', 'chat', 'memory', 'assistant', 'device'];

for (const dest of DESTS) {
  test(`screenshot ${dest}`, async ({ page }, testInfo) => {
    await seedToken(page);
    await openApp(page);
    await page.locator(`.tab[data-p=${dest}]`).click();
    // Prove the RIGHT pane painted before archiving - a shot of the wrong pane (dead
    // page JS leaving Home showing) must fail here, never be saved as evidence.
    await assertPane(page, dest);
    await page.waitForTimeout(600);
    const proj = testInfo.project.name; // desktop | phone
    await page.screenshot({ path: `screenshots/${proj}-${dest}.png`, fullPage: true });
  });
}
