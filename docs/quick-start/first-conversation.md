# Your first conversation

Once the setup wizard has finished, there are three ways to talk to the
assistant: Telegram, hold-to-talk on the device, and the browser chat. This page
walks through each one and ends with a short tour of the device's web page.

## Telegram (the main channel)

1. Create a bot with [@BotFather](https://t.me/BotFather) and paste its **bot
   token** into **Assistant → Connectors → Telegram** on the device's web
   page.
2. Restart the device (token changes are read at boot).
3. **Message your bot once** from your own account. Your message appears on
   the Telegram card as a **pending approval**. Click **Approve**.
4. Say hello. The assistant replies in text. With **Voice replies** on (the
   default, on the same Telegram card) it may also speak the reply on the
   device speaker when that fits; if the device cannot play it, the audio goes
   to Telegram instead. Spoken replies need a Mistral, OpenAI or Cumulo key;
   an Anthropic key alone has no voice.

The first approved account becomes the **owner**. Only the owner can run the
management commands such as `/update`, `/loops`, `/compact` and `/skill`; send
`/help` for the full list. Everyone else you approve can converse but not
manage the device.

:::caution Open access
The **Open access** checkbox lets anyone who finds your bot use it, and spend
your API credits. Leave it off unless you mean to run a public bot.
:::

## Hold-to-talk (on the device)

**Press and hold the on-screen mic bar**, speak, release. Up to 60 seconds per
hold. The ring and the screen always say what is happening:

| State | Ring | Screen |
|---|---|---|
| Listening (held) | your theme color: the LED ring breathes, the all-in-one's on-screen ring holds steady | "Listening" |
| Processing (released, until the reply lands) | a spinner in your theme color | "Transcribing", then "Thinking" with what it heard |
| Reply | back to normal status | the reply, held until you tap Close |

Processing starts the moment you let go, before anything goes over the network.

- **All-in-one**: the ring and the status lines are drawn on the screen. The mic
  button shows pressed while you hold and reads "wait" while processing. The
  on-screen spinner holds still while the recording uploads, then sweeps while
  the assistant thinks.
- **Nimbus board**: the LED ring shows the cue and the status lines replace the
  "Hold to talk" bar.

With **Voice replies** on, the assistant can also speak its reply on the device
speaker.

If something goes wrong, the screen says what, and the ring turns your theme's
alert color for 15 seconds (tap anywhere to clear it sooner, or hold the mic to
try again):

| You see | What happened |
|---|---|
| **No network**: Check Wi-Fi and try again. | The device is not on Wi-Fi (shown at once, without waiting for a timeout), or it is on Wi-Fi but the speech-to-text service cannot be reached (shown within about 10 seconds). The ring breathes. |
| **Mistral error** (or OpenAI, Cumulo): the HTTP status | The speech-to-text provider answered with an error, or did not answer in time. "Key rejected" means the key needs checking in the web app. |
| **Voice unavailable** | The provider refused the request, for example out of credit or rate limited. |
| **Busy** | Another request is running. Try again in a moment. |
| **No audio** | The mic recorded nothing. |
| **Didn't catch that** | The provider was reached and heard no speech. No alert color: nothing is broken. |
| **No answer** or the assistant's own error reply | The assistant could not finish the turn. |
| **No reply** | Nothing came back in time. |

Everything else is a **tap**: move between sessions, open the menu, and go back
from the touchscreen. On the Nimbus board the 45-LED ring shows live status at
a glance.

## The dashboard tour

Open the device's web page (scan the Sign-in QR on the screen if this browser
has not signed in yet):

- **Home** - health, battery, storage, and active sessions.
- **Chat** - a browser conversation with the same assistant, same memory.
- **Memory** - what the assistant remembers (your directive, its notes, and the
  long-term memories you can search) and every file it has saved.
- **Assistant** - one page with seven subtabs: provider keys and routing
  (Models), connectors like Telegram (Connectors), the tool surface and Tavily
  web search (Tools), approved skills (Skills), scheduled recurring tasks such
  as "morning digest at 8", each needing your approval before it can run
  (Routines), token usage and budgets (Usage), and download trust and guest
  screening (Safety).
- **Device** - mode and name, display, ring, sound, battery modes, software
  updates, connectivity, and the danger zone.

Try a request that uses more of the system: *"Remember that my partner's
birthday is March 12"* (a memory write you can see land under Memory), or
*"Research the best 2S BMS modules and send me a summary"* (a background
sub-agent you can watch on Home).

---

*How it works → [Turn anatomy - what the model actually sees](../turn-anatomy.md)*
