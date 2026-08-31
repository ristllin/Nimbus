<!-- audience: user -->
# Get your provider key

Orchestrator mode needs one AI provider key to think. This page shows, step by
step, how to create an account and generate a key with each provider, written so
you can follow it even if you have never used one before.

:::danger Get your key before you start setup
During setup, your phone or laptop joins the device's own Wi-Fi (`Nimbus-setup`),
which has **no internet**. You cannot open a provider's website to copy your key
while you are connected to it. So do this now, before step 1:

1. Create your key by following one section below.
2. **Paste it into the notes app on the same phone or laptop** you will run setup
   from (or a password manager). You want it one tap away when the wizard asks.
3. Then start [Set up the device](setup-wizard.md).

A key looks like a long line of random letters and numbers. Treat it like a
password: **never share it, post it, or email it.** Anyone who has it can spend
from your account.
:::

## Which provider should I pick?

Any one verified key runs the assistant. If you are not sure:

- **Easiest and free to try: Mistral.** Its free tier is enough to set up and use
  Nimbus, and no card is required to start. It is also the voice default.
- **One key for everything: a Cumulo key.** Prepay once and it works across
  Mistral, OpenAI, and Anthropic, with no per-provider accounts to manage.
- **OpenAI** adds a few extras (image generation, and spoken replies on the device
  speaker). **Anthropic** is great for conversation. Both need a paid balance to
  start.

You can add more providers later. You only need one to finish setup.

---

## Mistral (recommended start)

Free to try, no card needed to begin.

1. **Create an account.** Go to [console.mistral.ai](https://console.mistral.ai/)
   and sign up. Verify your email and phone number when asked.
2. **Billing.** Not required to start. The free tier covers first use. For higher
   limits later, open **Workspace**, then **Billing**, and add a card.
3. **Create the key.** Open [API Keys](https://console.mistral.ai/api-keys) (in the
   left menu under **API**). Click **Create new key**, give it a name like
   `nimbus`, and click to create it.
4. **Copy it now.** Copy the key and paste it into your notes app on the device you
   will set up from. This is your one chance to see it in full.
5. **To revoke it later.** Return to the same **API Keys** page and delete the key.
   The device stops working until you add a new one.

## OpenAI

Adds image generation and spoken replies on the device speaker.

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

1. **Create an account.** Go to
   [console.anthropic.com](https://console.anthropic.com/) and sign up. This opens
   the Claude Console (its address may show as `platform.claude.com`; that is the
   same place).
2. **Billing (required).** There is no free tier, so add money first. Open
   **Billing** (or **Plans & billing**) and buy a small amount of credit.
3. **Create the key.** Open **Settings**, then
   [API keys](https://console.anthropic.com/settings/keys). Click **Create Key**,
   name it `nimbus`, and create it.
4. **Copy it now.** Copy the key and paste it into your notes app on the device you
   will set up from. You see the full key only once.
5. **To revoke it later.** Return to the same **API keys** page and delete the key.

## Cumulo key (one key for all three)

Prefer a single prepaid balance instead of an account with each provider? A Cumulo
key draws on one balance and routes to any provider, so you top up once and use any
model.

1. **Create an account.** Sign in at
   [app.cumulo-nimbus.ai](https://app.cumulo-nimbus.ai).
2. **Add credit.** Top up your prepaid balance. You can set a monthly spending
   limit for a hard cap.
3. **Create the key.** Open **API keys**, create one, and copy it when it is shown.
4. **Copy it now.** Paste it into your notes app on the device you will set up from.
   You only see it once.
5. **To revoke it later.** Return to **API keys** and revoke it. Each key tracks its
   own usage, so you can keep a separate one per device.

Full details, including using the key from your own code, are on
[Use your Cumulo key](../cloud/cumulo-key.md).

---

Key in hand and pasteable? Continue to **[Set up the device](setup-wizard.md)**.

*How it works -> [Provider capability matrix](../reference/capabilities-matrix.md)*
