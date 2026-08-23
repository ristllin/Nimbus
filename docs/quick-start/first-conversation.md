# Your first conversation

Three ways to talk to the assistant: Telegram, hold-to-talk on the device, and
the browser chat.

## Telegram (the main channel)

1. Create a bot with [@BotFather](https://t.me/BotFather) and paste its **bot
   token** into **Capabilities → Connectors → Telegram** on the device's web
   page.
2. Restart the device (token changes are read at boot).
3. **Message your bot once** from your own account. Your message appears on
   the Telegram card as a **pending approval** - click **Approve**. No
   chat-ID hunting.
4. Say hello. The assistant replies in text - and, if **Voice replies** is on,
   sometimes with a spoken voice note.

The first approved account becomes the **owner**: only the owner can run the
management commands (`/update`, `/loops`, `/compact`, `/skill`, `/help` lists
them). Everyone else you approve can converse but not manage the device.

:::caution Open access
The **Open access** checkbox lets *anyone* who finds your bot use it - and
your API credits. Leave it off unless you mean to run a public bot.
:::

## Hold-to-talk (on the device)

**Press and hold the on-screen mic bar**, speak, release. While listening, the
Nimbus board's ring breathes red (the all-in-one shows the state on its screen),
then a spinner appears while it transcribes; the reply renders on the display
(and speaks, when a voice provider that supports the device speaker is
configured). Up to 60 seconds per hold.

Everything else is a **tap**: move between sessions, open the menu, and go back
from the touchscreen. The 45-LED ring on the Nimbus board wakes for a glance at
live status.

## The dashboard tour (two minutes)

Open the device's web page (scan the sign-in QR on the display if this browser
hasn't signed in yet):

- **Dashboard** - health, battery, storage, and live sessions. Watch a turn's
  ring arc mirror here while the assistant works.
- **Chat** - a browser conversation with the same assistant, same memory.
- **Capabilities** - provider keys and routing (Models), connectors like
  Telegram and Tavily web search (Connectors), and approved skills (Skills).
- **Memory & Files** - what the assistant remembers (vector memories,
  conversation history) and every file it has saved; search, preview, share,
  delete.
- **Routines** - scheduled recurring tasks ("morning digest at 8"), each
  needing your approval before it can run.
- **Settings** - battery modes, sound, connectivity, software updates, and
  the danger zone.

Try asking for something that exercises the machinery: *"Remember that my
partner's birthday is March 12"* (a memory write you can see land in
Memory & Files), or *"Research the best 2S BMS modules and send me a
summary"* (a background sub-agent - watch it on the Dashboard).

---

*How it works → [Turn anatomy - what the model actually sees](../turn-anatomy.md)*
