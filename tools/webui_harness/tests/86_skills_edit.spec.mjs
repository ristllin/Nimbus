// Lane I: the Skills panel edits staged revisions. The list shows each skill's
// one-line desc and what waits for the owner (a waiting skill, a proposed edit),
// the editor shows the proposed revision beside the live text with Approve edit
// and Discard edit, Save surfaces the server's warnings, and the "assistant may
// approve its own skill changes" toggle posts on|off.
import { test, expect } from '@playwright/test';
import { seedToken, openApp } from './_helpers.mjs';

const LIVE_MD = '---\ntitle: Meal\n---\nLog every meal with a range.';
const NEXT_MD = '---\ntitle: Meal\ndesc: read before logging a meal\n---\nLog every meal with a range and fibre.';

function listBody(pendingEdit = false) {
  return {
    sd: true,
    auto: false,
    skills: [
      { id: 'meal-photo-science', title: 'Meal photo science', desc: 'read BEFORE estimating a meal',
        source: 'sd', origin: 'user', inject: 'spawn', rev: 2,
        ...(pendingEdit ? { pendingEdit: true, nextRev: 3 } : {}) },
      { id: 'coffee-drinks', title: 'Coffee', desc: 'read BEFORE estimating coffee',
        source: 'sd', origin: 'agent', inject: 'spawn', rev: 1, pending: true },
    ],
  };
}

async function openSkills(page, listJson, getCalls) {
  const state = { approved: 0, discarded: 0, saved: null, auto: null };
  await page.route('**/api/skills/list', (r) =>
    r.fulfill({ status: 200, contentType: 'application/json', body: JSON.stringify(listJson) }));
  await page.route('**/api/skills/get**', (r) => {
    if (getCalls) getCalls();
    return r.fulfill({ status: 200, contentType: 'application/json',
      body: JSON.stringify({ id: 'meal-photo-science', source: 'sd', md: LIVE_MD, next_md: NEXT_MD }) });
  });
  await page.route('**/api/skills/approve', (r) => {
    state.approved++;
    return r.fulfill({ status: 200, contentType: 'application/json',
      body: JSON.stringify({ ok: true, outcome: 'edit-applied', rev: 3, message: 'The edit to meal-photo-science is applied (revision 3) and in use.' }) });
  });
  await page.route('**/api/skills/discard', (r) => {
    state.discarded++;
    return r.fulfill({ status: 200, contentType: 'application/json',
      body: JSON.stringify({ ok: true, outcome: 'edit-discarded', rev: 2, message: 'The proposed edit is discarded; the current version stays in use.' }) });
  });
  await page.route('**/api/skills/save', (r) => {
    state.saved = r.request().postData();
    return r.fulfill({ status: 200, contentType: 'application/json',
      body: JSON.stringify({ ok: true, outcome: 'replaced', rev: 3,
        message: 'Skill meal-photo-science is saved (revision 3).',
        warnings: ['add desc: one line saying WHEN to read this skill'] }) });
  });
  await page.route('**/api/skills/auto', (r) => {
    state.auto = r.request().postData();
    return r.fulfill({ status: 200, contentType: 'application/json',
      body: JSON.stringify({ ok: true, auto: r.request().postData().endsWith('on') }) });
  });
  await seedToken(page);
  await openApp(page);
  await page.locator('.tab[data-p=assistant]').click();
  await page.locator('.subtab[data-sp=skills]').click();
  await expect(page.locator('#skList')).toContainText('meal-photo-science');
  return state;
}

test('the list names every skill, its desc, what waits, and the approve or discard links', async ({ page }) => {
  const state = await openSkills(page, listBody(true));
  await expect(page.locator('#skList')).toContainText('read BEFORE estimating a meal');
  await expect(page.locator('#skList')).toContainText('an edit waits for your approval');
  await expect(page.locator('#skList')).toContainText('waiting for your approval');
  await expect(page.locator('#skList a[data-skapprove="meal-photo-science"]')).toHaveText('Approve edit');
  await expect(page.locator('#skList a[data-skdiscard="meal-photo-science"]')).toHaveText('Discard edit');
  await expect(page.locator('#skList a[data-skdeny="coffee-drinks"]')).toHaveText('Deny');
  // The default-off self-approve switch is unchecked.
  await expect(page.locator('#skAuto')).not.toBeChecked();
});

test('the editor shows the proposed revision beside the live text, and approve or discard post the id', async ({ page }) => {
  const state = await openSkills(page, listBody(true));
  await page.locator('#skList a[data-skid="meal-photo-science"]').click();
  await expect(page.locator('#skMd')).toHaveValue(LIVE_MD);
  await expect(page.locator('#skNextWrap')).toBeVisible();
  await expect(page.locator('#skNext')).toHaveValue(NEXT_MD);
  await page.locator('#skList a[data-skapprove="meal-photo-science"]').click();
  await expect.poll(() => state.approved).toBe(1);
  await expect.poll(() => state.discarded).toBe(0);
  await page.locator('#skList a[data-skdiscard="meal-photo-science"]').click();
  await expect.poll(() => state.discarded).toBe(1);
});

test('Save surfaces the revision and the server warnings, and the toggle posts on or off', async ({ page }) => {
  const state = await openSkills(page, listBody(false));
  await page.locator('#skId').fill('meal-photo-science');
  await page.locator('#skMd').fill(LIVE_MD);
  await page.locator('#skSave').click();
  await expect(page.locator('#skMsg')).toContainText('revision 3');
  await expect(page.locator('#skMsg')).toContainText('add desc: one line saying WHEN to read this skill');
  await page.locator('#skAuto').check();
  await expect.poll(() => state.auto).toContain('on');
  await page.locator('#skAuto').uncheck();
  await expect.poll(() => state.auto).toContain('off');
});
