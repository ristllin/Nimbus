<!-- audience: user -->
# Use your Cumulo key

A Cumulo key is one API key tied to a prepaid balance at
[app.cumulo-nimbus.ai](https://app.cumulo-nimbus.ai). Requests through it go to
the provider you name and are drawn from that one balance, so there are no
per-provider accounts or invoices to set up. Use this page to create a key, set it
on the device, and call the router from your own code.

## Create a key

1. Sign in at [app.cumulo-nimbus.ai](https://app.cumulo-nimbus.ai).
2. Add credit: on the **Credits** page (or with **Top up** on the **API keys**
   page), top up your prepaid balance. The router needs a balance before a key can
   make calls.
3. Open **API keys** and create one. Give it a name, and optionally a monthly
   spending limit for a hard cap.
4. Copy the key when it is shown. **You only see it once.** If you lose it, revoke
   it and create another.

Each key tracks its own usage and can be revoked on its own, so you can keep a
separate key per device or project.

## Use it on the device

The device can use your Cumulo key as its AI provider, so its assistant reaches
models without any other provider account.

1. In the device web UI, open **Assistant > Models**.
2. Under **Providers & keys**, paste the key into the **Cumulo Nimbus** row and
   select **Save & Verify**. In Notifier mode the button reads **Save Key**: the
   key is saved now and can be verified once you switch to Orchestrator mode.
3. Once it verifies, pick the **Orchestrator model** and **Session model**. The
   key is stored write-only: it shows as "set" and is never displayed again.

The model lists show only models the router can run, also right after an update
from an older version that saved a longer list: a router id that no provider can
run is never offered or saved, and a session model an older version saved that
way runs the default model instead. If the router refuses the
model you picked, the same message is answered on the last model that worked (or
on the router's default model), the reply opens with a line that names the model
and says to choose another under **Models**, and the device switches to the model
that answered so the next message works too.

You can also mint a key from the device instead of pasting one. The option appears
under **Device > Cloud access** once the device is paired with the cloud (see
[Cloud access](../cloud-relay.md); pairing needs Orchestrator mode), and only while
no Cumulo key is set (a Virtual Nimbus does not offer it). To mint one:

1. In the device web UI, open **Device > Cloud access**.
2. Under **Mint a key for this device**, enter **Capacity (credits per month)**
   and select **Save**.

The device creates a key for itself with that monthly cap, which renews each
billing period, and stores it the same way. You can revoke it anytime in the app.

### On a Virtual Nimbus

A Virtual Nimbus with only a Cumulo key runs the whole assistant through it, the
same as a device:

- **Verify and models.** The key is checked when you save it, and the model picker
  lists the models the router offers, grouped by provider (for example Anthropic's
  Claude models). A model you pick runs with the assistant's tools.
- **Memory.** Long-term memory works with only the Cumulo key: memories are saved
  and searched through the router.
- **Sub-agents.** Background research and other sub-agents run on the Cumulo key.
- **A model the router does not offer.** If the router refuses the model you
  picked, the reply says so and the assistant switches back to the last model that
  worked (or to the router's default model if that one is refused too), so the
  next message still gets an answer. Pick another model under **Models**.

## Use it from your own code

The router speaks the OpenAI and Anthropic APIs, so most tools work by changing
only the base URL and the key. Use these base URLs:

| Client style | Base URL |
|---|---|
| OpenAI-compatible | `https://app.cumulo-nimbus.ai/router/openai/v1` |
| Anthropic | `https://app.cumulo-nimbus.ai/router/anthropic` |

Set the key as the API key (bearer token). Examples:

```bash
curl https://app.cumulo-nimbus.ai/router/openai/v1/chat/completions \
  -H "Authorization: Bearer $CUMULO_KEY" \
  -H "Content-Type: application/json" \
  -d '{
    "model": "gpt-5.6-luna",
    "messages": [{"role": "user", "content": "Hello"}]
  }'
```

```python
from openai import OpenAI

client = OpenAI(
    base_url="https://app.cumulo-nimbus.ai/router/openai/v1",
    api_key="YOUR_CUMULO_KEY",
)

resp = client.chat.completions.create(
    model="gpt-5.6-luna",
    messages=[{"role": "user", "content": "Hello"}],
)
print(resp.choices[0].message.content)
```

```python
from anthropic import Anthropic

client = Anthropic(
    base_url="https://app.cumulo-nimbus.ai/router/anthropic",
    api_key="YOUR_CUMULO_KEY",
)

msg = client.messages.create(
    model="claude-haiku-4-5-20251001",
    max_tokens=256,
    messages=[{"role": "user", "content": "Hello"}],
)
print(msg.content[0].text)
```

The **Setup guide** at the bottom of the portal's **API keys** page shows the same
base URLs, with copy buttons and equivalent snippets.

## Costs and limits

- **Per-key spending limit.** Set a monthly limit on any key for a hard cap.
- **Per-key usage.** Usage is metered per key and shown in the portal.

## Related

- Reach the device from anywhere: **[Cloud access](../cloud-relay.md)**.
- Provider-attached tools (Gmail, GitHub, and more): **[Connectors](../connectors.md)**.
- Let a program on your network drive the device's tools: **[MCP](../mcp.md)**.
