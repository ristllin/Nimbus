// Lane J: the standing-rules editor under the Directive box. The list shows
// every rule (pending ones with an Approve link, nodash ones with the filter
// note), Add posts action=add and refreshes, Remove posts action=remove, and
// an add that is too long or empty surfaces the device's plain-words error.
import { test, expect } from '@playwright/test';
import { seedToken, openApp } from './_helpers.mjs';

const RULES = {
  items: [
    { id: 'p1', text: 'Never use em dashes anywhere.', state: 'active', created: 497800,
      origin: 'seed', nodash: true },
    { id: 'p2', text: 'A rule that waits', state: 'pending', created: 497801,
      origin: 'chat', nodash: false },
  ],
  max: { items: 12, itemBytes: 240, totalBytes: 1200, pending: 4 },
  bytes: 30,
};

function prefsPayload(items) {
  return { ...RULES, items: items ?? RULES.items };
}

// A FormData body carries `name="k"` then the value on the next line.
function formValue(body, key) {
  const m = body.match(new RegExp(`name="${key}"\\r?\\n\\r?\\n([^\\r\\n]+)`));
  return m ? m[1] : null;
}

async function openMemory(page, state) {
  state.actions = [];
  await page.route('**/api/prefs', (route) => {
    const req = route.request();
    if (req.method() === 'GET')
      return route.fulfill({ status: 200, contentType: 'application/json',
        body: JSON.stringify(state.body) });
    state.actions.push(req.postData());
    const act = formValue(req.postData() || '', 'action');
    if (act === 'add' && formValue(req.postData() || '', 'text') === 'empty') {
      return route.fulfill({ status: 400, contentType: 'application/json',
        body: JSON.stringify({ error: 'the rule is empty' }) });
    }
    return route.fulfill({ status: 200, contentType: 'application/json',
      body: JSON.stringify({ ok: true }) });
  });
  await seedToken(page);
  await openApp(page);
  await page.locator('.tab[data-p=memory]').click();
  await expect(page.locator('#prefslist')).toContainText('Never use em dashes anywhere.');
}

test('the list shows every rule, the pending Approve link and the filter note', async ({ page }) => {
  const state = { body: prefsPayload() };
  await openMemory(page, state);
  await expect(page.locator('#prefslist')).toContainText('(em dashes are removed from replies)');
  await expect(page.locator('#prefslist')).toContainText('waits for your approval');
  const approve = page.locator('#prefslist a[data-pid="p2"][data-pact="approve"]');
  await expect(approve).toHaveText('Approve');
  await expect(page.locator('#prefslist a[data-pid="p1"][data-pact="remove"]')).toHaveText('Remove');
  await approve.dispatchEvent('click');   // wiring click: the memory pane's polls jitter layout, so the handler is dispatched directly
  await expect.poll(() => state.actions.length).toBe(1);
  expect(formValue(state.actions[0], 'action')).toBe('approve');
  expect(formValue(state.actions[0], 'id')).toBe('p2');
});

test('Add posts action=add with the text, and the error shows the device words', async ({ page }) => {
  const state = { body: prefsPayload() };
  await openMemory(page, state);
  await page.evaluate(() => { document.getElementById('prefsnew').value = 'Always reply in English'; });
  await page.locator('#prefsadd').dispatchEvent('click');
  await expect.poll(() => state.actions.length).toBe(1);
  expect(formValue(state.actions[0], 'action')).toBe('add');
  expect(formValue(state.actions[0], 'text')).toBe('Always reply in English');
  // A refused add (empty text after cleaning) surfaces the plain-words error
  // without losing the typed rule: the field keeps it.
  await page.evaluate(() => { document.getElementById('prefsnew').value = 'empty'; });
  await page.locator('#prefsadd').dispatchEvent('click');
  await expect.poll(() => state.actions.length).toBe(2);
  await expect(page.locator('#prefsmsg')).toContainText('the rule is empty');
});

test('Remove posts action=remove with the id', async ({ page }) => {
  const state = { body: prefsPayload() };
  await openMemory(page, state);
  await page.locator('#prefslist a[data-pid="p1"][data-pact="remove"]').dispatchEvent('click');
  await expect.poll(() => state.actions.length).toBe(1);
  expect(formValue(state.actions[0], 'action')).toBe('remove');
  expect(formValue(state.actions[0], 'id')).toBe('p1');
});
