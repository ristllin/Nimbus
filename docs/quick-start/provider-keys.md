<!-- audience: user -->
# Get your provider key

Orchestrator mode needs one AI provider key. This page shows how to create an
account and generate a key with each supported provider, and what the device shows
after you paste it. Do this before you run setup.

:::danger Get your key before you start setup
During setup, your phone or laptop joins the device's own Wi-Fi (`Nimbus-setup`),
which has **no internet**. You cannot open a provider's website to copy your key
while you are connected to it. Before you start:

1. Create your key by following one section below.
2. **Paste it into the notes app (or password manager) on the same phone or
   laptop** you will run setup from, so it is ready when the wizard asks.
3. Then start [Set up the device](setup-wizard.md).

A key looks like a long line of random letters and numbers. Treat it like a
password: **never share it, post it, or email it.** Anyone who has it can spend
from your account.
:::

## Which provider to pick

Any one verified key runs the assistant. If you are not sure:

- **Easiest and free to try: Mistral.** Its free tier is enough to set up and use
  Nimbus, and no card is required to start. It is also the voice default.
- **One key for everything: a Cumulo key.** Prepay once and it works across
  Mistral, OpenAI, Anthropic, and Z.ai, with no per-provider accounts to manage. It also
  covers voice, both dictation and spoken replies, so a one-key device needs no
  separate voice provider.
- **OpenAI** adds image generation. **Anthropic** is strong at conversation and
  reasoning. Both need a paid balance to start.
- **Z.ai** runs conversation and tools only: no dictation, spoken replies, image
  generation, or memory embeddings.

You can add more providers later. You only need one to finish setup.

## After you paste a key

Tap **Save & Verify** (or **Verify** for a key that is already stored). The device
checks the key against the provider and shows a badge:

- **verified**: the key works. You can continue.
- **key rejected**: the provider did not accept the key. Check for a copy error and
  paste it again.
- **Needs credits or subscription**: the key is valid but the account has no
  balance. Top up with the provider, then tap **Verify**.
- **Couldn't reach the provider**: a network problem, not a key problem. Check
  Wi-Fi and tap **Verify** again.
- **Verification deferred**: the device was low on working memory, so it saved the
  key and skipped the check. Nothing is wrong with the key. It is checked again
  automatically when memory frees up, or tap **Verify** to retry now. Hover this
  badge to see the memory numbers behind it.

---

## Mistral (recommended start)

Free to try, no card needed to begin.

1. **Create an account.** Go to [console.mistral.ai](https://console.mistral.ai/)
   and sign up. Verify your email and phone number when asked.
2. **Billing.** Not required to start. The free tier covers first use. For higher
   limits later, open **Workspace**, then **Billing**, and add a card.
3. **Create the key.** Open [API Keys](https://console.mistral.ai/api-keys) (in the
   left menu under **API**). Click **Create new key**, name it `nimbus`, and
   confirm.
4. **Copy it now.** Copy the key and paste it into your notes app on the device you
   will set up from. This is your one chance to see it in full.
5. **To revoke it later.** Return to the same **API Keys** page and delete the key.
   The assistant stops answering until you add a new one.

## OpenAI

Adds image generation, and a second choice of voice for dictation and spoken
replies.

1. **Create an account.** Go to [platform.openai.com](https://platform.openai.com/)
   and sign up.
2. **Billing (required).** The API has no free tier, so a new key will not work
   until you add money. Open **Settings**, then **Billing**, add a payment method,
   and buy a small amount of credit (a few dollars is plenty to start).
3. **Create the key.** Open
   [API keys](https://platform.openai.com/api-keys) (under your profile menu, then
   **API keys**). Click **Create new secret key**, name it `nimbus`, and create it.
4. **Copy it now.** Copy the key and paste it into your notes app on the device you
   will set up from. It is shown only once.
5. **To revoke it later.** Return to the same **API keys** page and revoke the key.

## Anthropic

Strong at conversation and reasoning.

1. **Create an account.** Go to the Claude Console at
   [platform.claude.com](https://platform.claude.com/) and sign up.
2. **Billing (required).** There is no free tier, so add money first. Open
   **Billing** (or **Plans & billing**) and buy a small amount of credit.
3. **Create the key.** Open **Settings**, then
   [API keys](https://platform.claude.com/settings/keys). Click **Create Key**,
   name it `nimbus`, and create it.
4. **Copy it now.** Copy the key and paste it into your notes app on the device you
   will set up from. You see the full key only once.
5. **To revoke it later.** Return to the same **API keys** page and delete the key.

## Z.ai

Conversation and tools only. For voice, images, or memory embeddings, add a second
key (see [What works with which provider](setup-wizard.md#what-works-with-which-provider)).

1. **Create an account.** Sign up at [z.ai](https://z.ai).
2. **Billing (required).** Add credit to your account.
3. **Create the key.** Create an API key in your account and name it `nimbus`.
4. **Copy it now.** Copy the key and paste it into your notes app on the device you
   will set up from.
5. **To revoke it later.** Delete the key from the same page.

## Cumulo key (one key for every provider)

A Cumulo key draws on one prepaid balance and routes to any provider, so you top up
once and use any model without an account at each provider.

1. **Create an account.** Sign in at
   [app.cumulo-nimbus.ai](https://app.cumulo-nimbus.ai).
2. **Add credit.** Top up your prepaid balance.
3. **Create the key.** Open **API keys** and create one. Give it a name, and
   optionally a monthly spending limit for a hard cap.
4. **Copy it now.** Paste it into your notes app on the device you will set up from.
   You only see it once.
5. **To revoke it later.** Return to **API keys** and revoke it. Each key tracks its
   own usage, so you can keep a separate one per device.

Full details, including using the key from your own code, are on
[Use your Cumulo key](../cloud/cumulo-key.md).

---

With the key saved in your notes app, continue to
**[Set up the device](setup-wizard.md)**.

See also: [Provider capability matrix](../reference/capabilities-matrix.md) for what
each provider supports.
