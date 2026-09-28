# Your first conversation

Three ways to talk to the assistant: Telegram, hold-to-talk on the device, and
the browser chat.

## Telegram (the main channel)

1. Create a bot with [@BotFather](https://t.me/BotFather) and paste its **bot
   token** into **Assistant → Connectors → Telegram** on the device's web
   page.
2. Restart the device (token changes are read at boot).
3. **Message your bot once** from your own account. Your message appears on
   the Telegram card as a **pending approval** - click **Approve**. No
   chat-ID hunting.
4. Say hello. The assistant replies in text, and, with **Voice replies** on (the
   default), can also speak the reply aloud on the device speaker.

The first approved account becomes the **owner**: only the owner can run the
management commands (`/update`, `/loops`, `/compact`, `/skill`, `/help` lists
them). Everyone else you approve can converse but not manage the device.

:::caution Open access
The **Open access** checkbox lets *anyone* who finds your bot use it - and
your API credits. Leave it off unless you mean to run a public bot.
:::

## Hold-to-talk (on the device)

**Press and hold the on-screen mic bar**, speak, release. Up to 60 seconds per
hold. The ring and the screen always say what is happening:

| State | Ring | Screen |
|---|---|---|
| Listening (held) | steady ring in your theme color | "Listening", the mic shows pressed |
| Processing (released, until the reply lands) | a sweeping spinner in your theme color | "Transcribing", then "Thinking" with what it heard; the mic reads "wait" |
| Reply | back to normal status | the reply, held until you tap Close |

Processing starts the moment you let go, before anything goes over the network.
The reply also speaks when a voice provider that supports the device speaker is
configured. On the all-in-one the ring is drawn on the screen; on the Nimbus
board it is the LED ring and the lines appear in the mic bar.

If something goes wrong, the screen says what, and the ring turns your theme's
alert color for a few seconds (tap anywhere to clear it, or hold the mic to try
again):

| You see | What happened |
|---|---|
| **No network**: Check Wi-Fi and try again. | The device is not on Wi-Fi, or the speech-to-text service cannot be reached. Shown at once, without waiting for a timeout. The ring breathes. |
| **Mistral error** (or OpenAI, Cumulo): the HTTP status | The speech-to-text provider answered with an error. "Key rejected" means the key needs checking in the web app. |
| **Voice unavailable** | The provider refused the request, for example out of credit or rate limited. |
| **Busy** | Another request was using the connection. Try again in a moment. |
| **No audio** | The mic recorded nothing. |
| **Didn't catch that** | The provider was reached and heard no speech. No alert color: nothing is broken. |
| **No answer** or the assistant's own error reply | The assistant could not finish the turn. |
| **No reply** | Nothing came back in time. |

Everything else is a **tap**: move between sessions, open the menu, and go back
from the touchscreen. The 45-LED ring on the Nimbus board wakes for a glance at
live status.

## The dashboard tour (two minutes)

Open the device's web page (scan the sign-in QR on the display if this browser
hasn't signed in yet):

- **Dashboard** - health, battery, storage, and live sessions. Watch a turn's
  ring arc mirror here while the assistant works.
- **Chat** - a browser conversation with the same assistant, same memory.
- **Assistant** - one page with seven subtabs: provider keys and routing
  (Models), connectors like Telegram (Connectors), the tool surface and Tavily
  web search (Tools), approved skills (Skills), scheduled routines (Routines),
  token usage and budgets (Usage), and download trust and guest screening (Safety).
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
