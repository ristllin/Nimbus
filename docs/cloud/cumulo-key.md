<!-- audience: user -->
# Use your Cumulo key

A Cumulo key is one API key tied to a prepaid balance at
[app.cumulo-nimbus.ai](https://app.cumulo-nimbus.ai). Requests through it go to
the provider you name and are drawn from that one balance, so there are no
per-provider accounts or invoices to set up. Use this page to create a key, set it
on the device, and call the router from your own code.

## Create a key

1. Sign in at [app.cumulo-nimbus.ai](https://app.cumulo-nimbus.ai).
2. Open **API keys** and create one. Give it a name, and optionally a monthly
   spending limit for a hard cap.
3. Copy the key when it is shown. **You only see it once.** If you lose it, revoke
   it and create another.

Each key tracks its own usage and can be revoked on its own, so you can keep a
separate key per device or project.

## Use it on the device

The device can use your Cumulo key as its AI provider, so its assistant reaches
models without any other provider account.

1. In the device web UI, open **Assistant > Models**.
2. Under **Providers & keys**, choose **Cumulo Nimbus** and paste the key.
3. Once it verifies, pick a model. The key is stored write-only: it shows as
   "set" and is never displayed again.

You can also mint a key from the device instead of pasting one. Once the device
is paired with the cloud:

1. In the device web UI, open **Device > Cloud access**.
2. Under **Mint a key for this device**, enter **Capacity (credits per month)**
   and select **Save**.

The device creates a key for itself with that monthly cap, which renews each
billing period, and stores it the same way. You can revoke it anytime in the app.

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

The **API keys** page in the portal shows the same base URLs and snippets next to
each key.

## Costs and limits

- **Per-key spending limit.** Set a monthly limit on any key for a hard cap.
- **Per-key usage.** Usage is metered per key and shown in the portal.

## Related

- Reach the device from anywhere: **[Cloud access](../cloud-relay.md)**.
- Provider-attached tools (Gmail, GitHub, and more): **[Connectors](../connectors.md)**.
- Let a program on your network drive the device's tools: **[MCP](../mcp.md)**.
