# Changelog

Release notes for recent Nimbus firmware releases, newest first. Every release
ships as a signed image on the public
[nimbus-fw-releases](https://github.com/ristllin/nimbus-fw-releases)
repository; a device in Orchestrator mode sees it on its daily check and
installs it when you approve ([how updates work](ota.md)).

## Unreleased

- **Albums are answered once.** Several photos sent together get one reply that covers every photo, in the order sent, with the caption on the right photo. Before, each photo got its own reply, out of order, and a question sent right after an album could be answered before the last photos were looked at ("I didn't receive the third image"). A text sent right after the album joins the same reply; a voice note keeps its place among your messages.
- **A photo is never lost to a restart.** A photo used to be marked as received before it was looked at, so a restart at that moment lost it. Now nothing is marked received until its reply is done, and a photo that keeps crashing the device is skipped after the second restart (the rest of your message is still answered) with one message asking you to send it again.
- **Full captions.** A caption longer than 256 characters reached the assistant cut off. It now arrives whole.
- **Repeats cost one reply.** A message Telegram delivers again after it was answered, or the same message sent twice within 2 minutes before the first answer, is answered once, and you are told once. On 19 Oct one short message was answered 735 times.
- **An hourly limit.** The device answers at most 60 messages an hour, and 12 of them are always kept for your own chat, so a busy shared chat cannot use up your replies. When it pauses, it tells you once, with the time replies resume. Commands always work.
- **One download per photo.** The Telegram archive and the assistant used to download the same photo and voice note twice; they now share one download.
- **Late messages show their time.** A message or photo that arrives late, or is delivered again, carries the time it was sent, so the assistant does not take it for a new one.

## v4.5.16 - No more restarts every few hours

A fix release for a restart found on a desk device and reproduced on a bench device before the fix.

- **No more restarts every few hours.** The device restarted itself about every three and a half hours, and during busy moments more often. A routine or a message that called a tool for longer than about 8 seconds held the memory card, and the device's main loop waited for the card with no time limit until the watchdog restarted the device. The main loop now only tries the card for a moment and, when it is busy, skips that round of upkeep or keeps the event for the next round, so a long tool call can no longer restart the device.

## v4.5.15 - Working memory is back

A fix release on top of v4.5.14, found by the release test runs on two bench devices before v4.5.14 reached any desk device.

- **Working memory is back.** v4.5.14 started with about 4.5 KB less free working memory than v4.5.13, so every message had that much less room before the device refused it. Twelve long-lived buffers (the web memory echo, the system message prompt, the connector sign-in handoff and the pending Telegram voice, attachment and media slots) now live in the device's large external memory. Each request now starts with slightly more free working memory than on v4.5.13: on the test device 26.3 KB against 26.1 KB, and 42.5 KB against 42.2 KB at idle.
- **A night cut short by a restart says so.** When a restart interrupted the nightly upkeep, its routine record showed a failure with no reason. It now reads "the device restarted during this run".
- **A crash is not blamed on an older crash.** When the newest crash left no saved report, the boot row named the cause of an earlier one. It now says the cause is unknown.

## v4.5.14 - No repeated messages, no silent losses, a reason for every failure

Found on a desk device between 13 and 21 Oct, and reproduced on a bench device before each fix. Its small settings storage filled up on 14 Oct, and from then on several things went wrong together:

- **Telegram repeated itself.** The device could no longer store which messages it had handled, so Telegram served the same ones again every few seconds: a photo was taken in 17 times, one batch of messages was answered 11 times, and on 21 Oct one page of six messages came back 50 times, each time as a paid turn and a duplicate notice.
- **Saves failed in silence.** Settings answered "saved" when nothing was stored, and most of October's spend and every change to the assistant's notes lived only until the next restart.
- **Memory was lost.** Facts you asked it to remember were thrown away while the embedding provider (the one that indexes memories) was out of credit (8 of them on 13 and 14 Oct), and after a restart a clean-up pass deleted the photos, voice notes and full tool outputs of older messages.
- **Failures had no reason.** A crash left no readable cause. On 18 Oct the nightly dream failed on Anthropic, then on the two providers it switched to, and only the last provider's bare "HTTP 400" was kept; asked why, the assistant guessed.
- **Spend ran on.** A provider that was out of credit was asked again on every message, a wrong model name was sent two to four times, and every turn resent the same 30 KB of instructions at full price. A turn that switched providers filed every provider's spend under the last one, and a failed turn's spend was not counted at all.
- **Replies claimed work that never happened.** Between 16 and 18 Oct replies said a plan was written, research had started or a note would survive a restart when none of it was true, and six replies promised 34 research helpers that never started. The nightly dream sent its error notices to Telegram at about 03:30.

### What changed

Telegram and the settings storage:

- **Messages are never handled twice because storage is full.** The device remembers which Telegram messages it has handled in memory while it runs and only keeps a copy in its settings storage for restarts. When that storage is full, Telegram still learns the messages were handled, so nothing repeats. A restart right after a message can repeat that last batch once.
- **Commands in a burst run as commands.** Several messages sent quickly are still answered together, but a command among them (such as `/skill approve`) now runs on its own, in order, instead of being read as part of the conversation. If the device restarts in the middle, it repeats at most the message it was handling at that moment, never a command it had already finished, and a forwarded message that starts with `/` is read as a message, never run.
- **The device backs off when Telegram answers with an error.** Any error answer, including "too many requests", now makes the device wait before it checks for messages again, for at least as long as Telegram asks, instead of asking again every second.
- **The settings storage keeps room for what keeps the device safe.** The last 24 free entries are kept for the Telegram message position, the update rollback guard and the cloud sign-in, so a filling device no longer repeats messages or loses the ability to roll back a bad update. Background saves (caches, counters) are refused first, and rebuildable caches are cleared to make room. If even that is not enough, your own settings still save and only background saves wait, down to a last reserve of 8 free entries for the Telegram position and the update safety net: a save that would take one of those is refused too, and the device tells you ("storage full").
- **Spend and the assistant's notes are kept.** The usage ledger and the assistant's scratchpad were rewritten in the small settings storage after every message, so once it filled, their changes lived only until the next restart. Both now live in files in flash. The first start of this release carries them over, and the old copies are removed only once this release has proven itself (an update that rolls back still finds them). A save that fails is retried until it lands.
- **Saves that did not happen say so.** Removing, renaming, approving or promoting a Telegram chat, the fallback rules, the scratchpad and the memory settings now answer "storage full" when the change could not be stored, instead of a silent "saved". Removing a chat can no longer leave it as an owner, and only chats on the allowlist keep a display name.
- **See what fills the settings storage.** The device tells you once on Telegram when a setting could not be saved, the device log names the setting that failed, and the web API lists every stored setting with its size (never its value). The assistant's device status reports the storage's free space, the saves that failed since boot, the last setting that failed to save (never its value) and whether the Telegram read position was saved, so when the same answer repeats it can point at the device instead of guessing about your phone or app.
- **An update never starts without its safety net.** If the rollback guard cannot be stored, the update stops with "storage full" instead of installing an image that could not roll back, and a crash-looping image now rolls back even on a full storage.
- **Helper jobs survive a full storage.** Tidying the job list could erase a running job's record when the storage was full; running jobs are now never moved or erased.

Memory and history:

- **Facts you ask it to remember survive a provider outage.** When the embedding provider refuses (out of credit, key rejected, offline), the fact is kept on the device and stored automatically once the provider works again. Every save ends as stored, waiting or not stored, and the assistant is told which, so it no longer says "saved" for something that was not. While the provider keeps refusing, the device stops calling it on every message (each doomed call delayed the reply by 1 to 2 seconds), still finds the waiting facts by keyword (without copying the whole memory store, and skipping that search with a note when the device is low on memory), and says when its recall is keyword-only.
- **Attachments survive a restart.** After a restart the device looked at only the newest few hundred saved messages, and the next clean-up deleted the attachments of every older one. It now deletes no attachment until it has looked at every saved message: right after a restart it works through the rest in the background, in small steps, pausing while a reply is written. The clean-up never runs during a reply, and history files still age out after 30 days. A search of old history reaches about three times further per step and says how far back it got.
- **Every Telegram message is kept on the SD card.** Each message as it arrived, every photo at the full size Telegram has, voice notes, documents, videos and stickers (files up to 20 MB), and every reply in full before it is split into parts. Chats not on your allowlist are kept as text only. Nothing is deleted by age and the clean-up never touches it; if the card runs low, the oldest days go first (chats not on your allowlist before your own), the newest day of your own chats is always kept, and you get one message about it. Read it through the device's web API; the files also show, read-only, under Memory, then Files.
- **Memory answers that tell the truth.** Pinning a memory that is already pinned says nothing changed. A note that could not be saved says it lasts only until the next restart, on the web page too ("kept until restart only"), and the assistant's notes say so until saving works again. Since the notes now live in a file in flash, a full settings storage no longer stops them saving.
- **Memory search results stay in their conversation.** Results of a memory search went to whichever conversation spoke next; they now go back only to the conversation that asked. On a device shared with other people, the same fact saved by two people is kept for each of them.

Spend and providers:

- **Each provider's spend goes to that provider.** When a turn moves between providers, each one's tokens, cache reads and writes included, are filed under the provider and model that billed them, and the turn's summary lists each of them. A turn that fails after some rounds still counts what those rounds cost, and a routine or the dream reports its own spend, never the previous turn's.
- **An out-of-credit provider is set aside.** Once a provider refuses for lack of credit or rejects its key, it is skipped, and later turns go straight to the next provider until you change its key or model, or a check 30 minutes later succeeds. This holds on every kind of turn, plain single-answer turns and conversation compaction included, and an out-of-credit provider gets one notice per episode, not one per turn; memory saves on the same provider share that notice. A bad request is never repeated: a wrong model name gets one request, not two to four.
- **Helpers never fall back to the priciest model.** A research helper asked for a model its provider does not offer now runs on the cheapest model that provider lists, never on whichever is listed first (often the most expensive), and the assistant is told the swap, including when no model was named. If none of the listed models has a known price, the helper does not start and you are told why.
- **Repeated instructions cost about a tenth.** The instructions sent with every turn are now ordered so the part that changes only with your settings or the day comes first and the per-message part last, and providers bill the repeated part at about a tenth of the price. On a test pair of turns a minute apart, the second read 99% of it from the provider's cache (about a third before).
- **Cache pricing for OpenAI and current prices.** OpenAI's cached and cache-written tokens are now counted and priced at each model's own rates (0.1x and 1.25x of the input price where there is no rate). New built-in prices: Claude Haiku 5.5, Sonnet 5.5, Opus 5.5 and Fable 5.1, GPT-5.6 (at its regular price; its promotional price runs until at least 21 Nov), GPT-6 Luna and Sol, GPT-6.1 Sol, Mistral Medium. A model with no built-in price is estimated at its provider's highest price, never a cheaper one.
- **Providers refuse less.** Every text sent to a provider is valid UTF-8 and within its size limit, so one broken character no longer makes every provider refuse the whole turn. Claude now chooses its own tool calls on every model, so Sonnet 5.5, Opus 5.5 and Fable 5.1 no longer refuse the final round, and a plain-text final answer is accepted. Claude, OpenAI, Mistral and custom endpoints share one 120-second limit per round (Z.ai keeps its 50 seconds), and Mistral errors now say why.

Crashes and low memory:

- **Every crash leaves a reason.** The next start writes down which part of the device stopped, where, and the exact error. The record stays on the device and shows in the device log, the device status and the history the assistant reads, so it can say why it restarted. The log keeps its first lines on every start, and routine network chatter no longer pushes the lines that lead up to a crash out of the log within hours.
- **After a crash, one message says what was lost.** It says in plain words what went wrong, names the request that was cut short (for example your photo from 12:30) and asks you to send it again. A crash during a routine or a background task, such as the nightly memory upkeep, waits for the next time you talk to the device instead of messaging you at once.
- **Low memory is handled, not crashed into.** Every failed memory allocation and every new memory low is recorded in the device status with what was running at the time. The cloud dashboard's status updates go straight out while a reply runs; only a large cloud page shares the device's one secure connection with the reply, and it waits its turn or the cloud shows a short busy page and retries. Large pages on your own network are sent in small pieces while a reply runs, background work starts only when there is memory for it, and a provider check put off for lack of memory runs once memory recovers.

The nightly dream:

- **No more messages at night.** When the nightly memory upkeep (the dream) failed, its error notices were sent to Telegram at about 03:30. Background turns now keep their notices to themselves; the result is recorded in the timeline instead. When the dream has something to tell you, it now waits for your next conversation instead of arriving at night.
- **The dream says what it did.** The timeline row now reads ok only when every memory it saved landed, and gives the counts: how many were stored, updated and failed. A failed night names the cause; a deferred night says when it will try again.
- **A failed night tries again, a few times.** A night that was deferred for low memory, or failed for a passing reason, retries 15 minutes later, up to three times before 07:00. Repeated failures no longer switch dreaming off; after three failed nights in a row you get one message the next time you talk to the device.
- **A restart no longer hides a night.** If the device restarted during the nightly memory upkeep, the night looked like it never happened: the previous night's result stayed, nothing was written down and no retry was made. The device now notices the interrupted night at startup, records it as failed in the timeline, and tries again the same night while it is not too late. A damaged state file or an old leftover from an earlier night is ignored, never reported as a new failure.
- **Memory only.** The dream can read and save memories and tidy the scratchpad, and nothing else: no files, helpers, web searches or routines in the middle of the night.
- **Storage trouble is said, never looped on.** If the device's internal storage is not mounted or refuses a write, the dream still finishes, still stops after three tries and still tells you about a failing streak; its row ends "state not saved" with the reason, and the health panel shows an Internal storage row. Saving settings or chat state on unmounted storage now fails at once instead of filling the log.

Replies and failures:

- **Replies say only what happened.** Before a reply goes out, the device now compares what it says was done (saved, written, started, sent, pinned, survives a restart, checked live, a figure read from a label) with what the turn actually did and saw. When they disagree, the assistant first gets one chance to do the work or rewrite the reply; when that cannot run or does not fix it, the reply ends with one short line, for example "(Correction: that has not been saved.)".
- **Promised research helpers start.** When the assistant says it started research helpers, they now start, or you are told plainly that they did not, and anything in its answer the device cannot understand is reported instead of dropped. That holds for an answer that was cut off or could not be read too: if the part you receive says helpers were started, it ends with the line saying none were. The assistant sees which helpers it started, which finished and which were refused and why, from the device's own job list.
- **A failed turn says why.** The first thing that went wrong is kept, along with every provider that failed after it, and it now reaches every place a failure shows: the reply ("That didn't finish - anthropic: ... (openai and mistral also failed)"), the turn's summary in the chat, the device timeline, the dream's line, the routine's record and the assistant's device status, so it can answer "why did that fail" from what was recorded. A turn where no provider answered is no longer reported as a provider switch.
- **A failed turn says it plainly.** The closing notice now names the first cause in the assistant's own words, never the provider's raw error line, and it suggests asking again only when asking again can help (a busy server or a dropped connection, not a refused request). When helper jobs are still running, the notice says so and that their results will arrive, and it no longer claims nothing is running.
- **Failures explained from evidence.** The assistant is told to give only causes it can show, and to say when the cause is not recorded instead of guessing.
- **The turn summary is always written.** A long summary used to be dropped without a trace; it now shortens to fit.
- **Research results say whether anything was looked up.** A helper's answer to a live question (a status, today's weather, prices) that ran no lookup is marked as not checked, and the assistant sees how many tools and web searches each helper ran.
- **Cleaner replies.** Leftover tool markup, private reasoning and wrapper tags never reach a chat. An answer cut off at the length limit is retried once with more room; if it is still cut off, none of its actions run and you get the part that finished with a short note that it was cut off.

Smaller fixes:

- **Battery health no longer blocks an update.** Battery health is an estimate learned from use, and a desk device that rarely runs down can read low without a way to fix it. It is now labeled an estimate, never stops an update, and a low reading only adds a note to keep the device on power while it updates.
- **Fewer storage errors on Freenove and CYD boards.** On boards whose SD slot is SDMMC, the device tried to download the larger sound set through the wrong card driver, and logged a storage error every second or two, forever. It now keeps to the built-in sounds on those boards.
- **Cheaper meal logging.** Logging a meal no longer reads and rewrites whole files: a new "add rows" action writes only the new line, and a "last rows" read looks only at the end of a log.
- **Hosted Virtual Nimbus.** With no Anthropic model picked, the hosted assistant now uses Claude Haiku 5.5 instead of Claude Sonnet 4.6. A model you picked is unchanged.

### What you will notice

- One answer per message, even when the settings storage is full, and a command sent in a burst gets its own answer, in order.
- The cloud dashboard keeps updating while the assistant is replying, instead of stalling for up to 20 seconds and then showing busy.
- A research helper asked for a model its provider does not offer runs on the cheapest model that provider lists (the assistant is told which), not the most expensive one.
- At most one Telegram message per start when a setting could not be saved: "settings storage is almost full, so some changes may not be saved. To free space, open the device web page and remove connectors or provider keys you don't use."
- "storage full" in place of "saved" when a change could not be stored.
- When a provider runs out of credit, one message that names it and the provider answering instead ("... says the account is out of credit. Add credit on its billing page; until then, mistral answers."), and no more refused calls to it.
- After a crash, one message such as "Nimbus restarted after a crash while answering your photo from 12:30. That reply was lost, so please send it again if you still need it."
- No messages at night: anything the nightly upkeep has to tell you arrives with your next conversation. After three failed nights in a row, one message the next time you talk to the device: "Nightly memory upkeep has not worked for 3 nights in a row (...). It keeps trying each night. To pause it, send /loop off dream."
- A short "(Correction: ...)" line at the end of a reply that claimed something the turn did not do.
- Asking whether a fact was saved while the embedding provider is down gets an honest answer: it is waiting, and it is stored later on its own.
- Battery health shown as "Health (estimate)".
- "Updated to v4.5.14" on Telegram once the update has proven itself.

### Before and after the update

- **Before, on a full device.** v4.5.13 cannot tell when the update's rollback guard fails to save, so on a device that already reports a full settings storage, remove one connector or provider key you do not use first (Assistant, then Connectors or Models). If the battery health reads low, v4.5.13 asks before it installs: choose Install Anyway while the device is on power.
- **Nothing to set afterwards.** The install takes about two minutes, and the update counts as proven after two more minutes of healthy running, when "Updated to v4.5.14" arrives on Telegram.
- **Restart once to give back the old copies' space.** The spend record and the assistant's notes move into files at the first start, but their old copies stay in the settings storage until the first restart after the update has proven itself, so a return to v4.5.13 still finds them. After "Updated to v4.5.14" arrives, restart from the web page (Device, then Restart); a nearly full device gains the most room.

## v4.5.13 - Prices and usage per model

Asked for by the owner: prices were set per provider, though models from the same provider cost differently, and the usage graphs showed providers only. Checked on a desk device over turns on three router models and by four adversarial review rounds over the change and the release.

- **Prices per model, usage per model.** Each model now has its own price: two models from the same provider no longer cost the same, and a provider's dollar budget adds them up. The Usage page lists this period's spend per model, the Spend over time chart shows models (or providers, your choice), and Prices takes dollars per million tokens for each model, with a last row that sets the provider's default in place of the estimates. Cumulo usage is priced at what Cumulo bills for each model. Models you have not priced use built-in estimates, marked ~.
- **A dollar budget now counts built-in estimates.** A dollar cap used to count only the prices you had entered, so a cap with no prices never stopped anything. It now counts every model at its price, built-in estimates included, and stays off only for a model nothing prices (such as a custom endpoint model you have not priced). If you set a dollar cap without entering prices, check it under Usage, Budgets: the cap may now stop a provider when its estimated spend reaches it.
- **Going back to an earlier release.** An earlier release keeps your usage, budgets and provider prices, and reads the per-day history (each day's spend is still counted). It ignores the per-model prices and per-model spend, which come back if you update again. Spend recorded while the earlier release ran has no model, and that release may also have reset its settings, so after the update that spend is priced at the provider's highest known price; when the device can tell the earlier release ran (it rewrote the history), the whole billing period of each provider it used is priced that way (the per-model rows start again with your next turn). The history it rewrites while you run it records days without the model.
- **A storage failure never makes spend cheaper.** If the device could not save its usage (settings or flash storage full) and then restarts, spend it can no longer tie to a model is priced at that provider's highest known price, never at a cheaper default, so a dollar cap cannot lift early. The same goes for a model the device could not keep a row for, and for this period's spend after Erase Storage on a device with no SD card. Usage from before this update is priced at your default for that provider, else the highest price you or Cumulo set for one of its models, else the built-in estimate.
- **Spend goes to the model that answered.** When an attempt failed partway through a turn and another attempt answered (the same provider again, or another one), the turn's spend is filed under the model that answered, never under a model the failed attempt named.
- **Easier to read amounts.** The Spend over time chart's lines land on round amounts, and money shows two decimals unless an amount is under a cent.
- **Cumulo prices right after an update.** A device with a verified Cumulo key now fetches what Cumulo bills for each model on its own shortly after it starts, instead of showing estimates until you press Verify.

## v4.5.12 - Updates on a busy device, a reset that erases, keys that apply at once

Found by hardware checks on two desk devices (a full factory reset read back from raw flash, a device with Telegram and the cloud link that could not update), a live check of a fresh hosted Virtual Nimbus on v4.5.11, and adversarial reviews of each change and of the whole release.

- **A busy device updates over the air again.** A device with Telegram, the cloud link and audio on could refuse every update check with "Can't check now: low memory", the daily one included. When the update cannot get memory of its own, it now runs on the assistant's own task between messages; while an install runs, new messages wait. Nothing is paused or restarted, so Telegram keeps working afterwards.
- **A full factory reset erases the internal storage.** Deleted routines and chat state used to stay readable in the internal storage after a full reset to anyone with the board and a flashing tool. A full reset now erases all of it before it clears the settings; if the erase cannot finish, the reset stops and the device keeps its settings. Files on the SD card are deleted but not overwritten: to hand a device over, remove the card or format it on a computer with an overwriting format ([security](security.md)).
- **A key added later works at once.** A Cumulo, Z.ai or custom endpoint key added on the web page after the device started left research helpers failing with "isn't configured" until a restart. It now applies to the next helper.
- **Helpers on the Cumulo router get time to finish.** A router helper now has 120 seconds to answer (Z.ai 50 seconds, under its own cutoff) instead of 30, so a normal brief on a reasoning model no longer times out and then passes on a retry.
- **Answers from reasoning models are read.** A model that sends its answer after a "thinking" part (Mistral's mistral-large-4-0, Anthropic with thinking) was reported as "couldn't read its result" after it had run. Every reply reader now takes the answer text.
- **A quieter setup mode.** A new device waiting for Wi-Fi no longer logs a settings error every five seconds.

## v4.5.11 - Helpers that run on your Cumulo key

Found by a live check of a fresh hosted Virtual Nimbus on v4.5.10 with only a Cumulo key.

- **Research helpers run on your Cumulo key.** When the assistant asked for a helper on a provider you have no key for, the helper failed even though the Cumulo router offers that model, and the assistant had already said it started. A helper now runs on the router when you have no key of your own for its provider, on a desk device and on a hosted Virtual Nimbus. A connector task (your calendar, inbox or workspace) never runs on the router, because the router cannot use your connectors; it is refused with the provider that can.
- **A hosted Virtual Nimbus knows where a message came from.** A message typed on its web page is described to the assistant as the web chat, not Telegram, so replies and reminders go back to the page.
- **Hosted settings that do not apply say so by name.** Device-only switches on a hosted page (such as automatic updates) are refused with their real name instead of an unreadable message.

## v4.5.10 - A hosted Nimbus that keeps its promises, helpers that say what they did

Found by a new joiner's journey on a fresh hosted Virtual Nimbus with only a Cumulo key, a second journey on a freshly provisioned desk device, a page-by-page review of the public docs, and six adversarial review rounds over the change set.

- **A hosted Virtual Nimbus now behaves like a device.** A Claude or Mistral pick over the Cumulo router runs that provider's own API with the full tool loop, so memory saves, searches and research actually run instead of being narrated. With only the Cumulo key, memory embeddings and research helpers run through the router, the assistant knows the real time in your time zone and that it is an online hosted instance, and the key verifies so the model picker fills.
- **Hosted settings apply or say no.** Primary provider, fallback order, directive, time zone and Clear conversation take effect and read back; `/clear` clears without a model call; Files work on the instance disk; routines, skills and wake-ups answer "Not available on a hosted Nimbus" instead of a false "Saved". Usage counters survive key and model saves, the hosted page gains the last-turn, log and error-log views, and those views mask keys. A key saved earlier with a surrounding space is kept, trimmed.
- **A refused model gets an answer, not a dead assistant.** If the Cumulo router does not offer the model you picked, the turn is answered by the model you used last, or the router default, and the reply says so. On a desk device your Orchestrator pick then moves to the model that answered, so choose another under Assistant > Models if you want a different one. The Cumulo list keeps the router default and one model of each provider family, drops ids the router cannot serve, and keeps a pick's real context window.
- **Helpers say what they did.** A Mistral helper asked for your calendar or inbox that never ran the connector opens its answer with that fact, and nothing from it is written to memory as a fact. Text that merely uses the words Linear, Notion or Slack is not mistaken for a connector request. A helper that may have started is never spawned again, on any provider, on a device or a hosted instance. When the model calls a reply field or a connector name as a tool, the error tells it the real way back.
- **Z.ai answers in time.** It now asks for bounded reasoning and waits at most 50 seconds instead of over four minutes, and says so when the provider did not answer.
- **Fewer wrong turns on the device.** The assistant describes the board it was built for. A request the device could not finish sending because memory ran low is sent once more after memory recovers, and the message says memory, not connection. A device without a Telegram bot, or in public mode without an allowlist, is never steered toward a Telegram tool it cannot deliver with, so a web reminder arrives without an apology. Saved helper reports start at the report.
- **Web page and connectors.** Saving a Mistral Studio connector checks your Mistral workspace right away and on boot; the connector card keeps what you are typing while it checks; a key with a hidden character is refused with a sentence that now reaches the page; the Wake-ups control saves; the Factory Reset dialog says what it keeps; wording points at the real page locations.
- **Docs re-checked against the firmware.** 167 claims on 43 public pages were verified in the code and 161 corrected, including the battery cell count (a setting), the touch calibration location, auto-install behind your opt-in, the three Freenove panel images and the Linear connector sign-in.

## v4.5.9 - Honest saves, reminders that reach you, helpers that start

Found by two invented new joiners running 15 to 20 turn sessions on fresh boards, a blank-machine walk through the docs, and an adversarial review of the whole change set.

- **Saving settings is honest again.** On a device with several provider keys the settings store could fill up, and the web page still said "Saved" while nothing landed (connectors, provider switches, scratchpad). Every save now reports the failure (HTTP 507 with a plain next step), provider model lists moved out of the settings store into flash, and an already-full device frees the space on its first boot of this version.
- **Reminders and routines reach the place you asked from.** A reminder set in the web chat used to fire into an empty Telegram list; it now lands in the web chat (and the open page picks it up without a reload).
- **The clock sets itself on every boot.** On a fast Wi-Fi join the time sync was skipped, so daily routines never ran and the device did not know the date.
- **Research helpers finish properly.** Anthropic helpers failed to start; results that arrived minutes apart were dumped raw instead of summarized; helper notes and summaries now stay in the chat history. The Z.ai host answers in its own words instead of a formatting apology, "remember these facts" writes every fact before it says so, a previous code answer reaches the next turn whole, and a Tavily key enables web search immediately.
- **Web chat replies pair with their own message.** A failover notice or a late summary can no longer appear under the next question; /clear starts a genuinely fresh conversation.
- **Memory search finds partial matches**, best match first, instead of requiring every word.
- **Installer and build.** The command-line installer keeps its board and screen checks on every Python, picks the right Freenove image for the screen size, and the firmware's web-server libraries are pinned so a tagged release rebuilds to the same bytes. Docs for a first install on Linux, macOS and Windows were corrected end to end.

## v4.5.8 - A factory reset you can trust, voice that starts now, memory on Mistral

- **Factory reset now erases everything, and says so first.** Settings > Reset opens a
  choice: "Reset settings" returns every setting to its default and keeps your Wi-Fi,
  keys, and memories; "Factory reset" erases all of it - Wi-Fi and passwords, provider
  keys, paired computers, settings, memories, routines, and logs, including the SD
  card - with the full list on screen before you confirm. The erase refuses honestly
  if the card holding your memories is not readable ("Reseat the SD card and try
  again"), and the confirm button needs a second deliberate tap, so a bounced touch
  can never erase the device.
- **Voice and web messages start right away.** A hold-to-talk or web message used to
  wait for the next Telegram check, up to half a minute. It now starts at once, and
  Telegram messages still arrive as before.
- **Two devices no longer end up with the same name.** A second Nimbus set up next to
  a running one now calls itself Nimbus-2 - its setup network and web address stay
  distinct.
- **Mistral works for memory.** Choosing Mistral as the memory (embeddings) provider
  now works; before, every save failed. If the memory store and the model disagree on
  size, the device says exactly that instead of pretending nothing matched.
- **Rate limits name their window everywhere.** A Virtual Nimbus and background
  helpers now say which limit was hit and when it resets, the same way the device
  already did.
- **Behind the scenes:** the release pipeline's remaining actions are pinned, and a
  weekly full build of every firmware variant guards against silent rot.

## v4.5.7 - Voice that responds instantly, setup that signs you in, connectors that really work

- **Letting go of the mic responds instantly.** Releasing the hold-to-talk button now
  changes the screen and ring at once, shows a thinking state while your request is
  processed, and ends with the reply. Off Wi-Fi, the answer comes back in about two
  seconds and says the real reason: "No network. Check Wi-Fi and try again." Every
  failure now names itself (provider error, nothing heard, busy) instead of a
  catch-all "didn't catch that".
- **Publish Setup Network now walks you through it.** The button confirms what happens
  next, and the device screen switches to the setup network: the network name and
  password, a join QR, numbered steps, and a sign-in code that the sign-in page
  actually accepts. Tap the code to enlarge it. The code field on the phone no longer
  capitalizes what you type, and every card fits the screen.
- **Wi-Fi status is back on the screen.** The header shows connected, searching, or
  not set up, plus an amber AP marker while the setup network is up.
- **Mistral connectors work, verified against your Mistral account.** The device now
  asks Mistral which connectors your account really offers and uses exactly those;
  Google Calendar, Notion, and Slack pulls were proven end to end. Connector turns no
  longer stall for a minute, and a rate-limit reply tells you the actual limit you
  hit and when it resets.
- **A Virtual Nimbus now matches the device.** It runs sub-agents, uses the same
  connectors, and handles Telegram the device way: nobody is trusted by default, and
  you approve each chat from its own web page with the roles you already know.
- **The sign-in page keeps everything behind it hidden** on the phone until you sign
  in, and the chat no longer piles up old tool-call disclosures without their
  conversation.

## v4.5.6 - Stuck key checks clear themselves, clearer Mistral connector setup

- **A key check stuck on "Verification deferred" now recovers, and Verify works for
  every provider.** The Cumulo Nimbus and Z.ai keys were missing from the manual
  re-check, so their Verify button did nothing, and a check deferred during a
  low-memory moment could be abandoned and never retried, leaving the key stuck
  "deferred" for good. The check now retries itself once memory frees, and Verify
  re-runs it on demand for every provider.
- **Mistral connectors tell you where to finish setup.** Turning on a Mistral
  connector (Google Calendar, Notion, Slack, or a hosted tool) now says plainly that
  it is authorized in your Mistral account (La Plateforme > Connectors), not on the
  device: add your Mistral key here, then turn the connector on in Mistral. Before,
  an enabled-but-unauthorized connector looked on but did nothing, and the hint
  pointed at the wrong place.

## v4.5.5 - More free memory on the Freenove, clearer key and memory checks, a Safety activity view

- **More free working memory on the Freenove board.** The audio parts now release
  their buffers when sound is idle instead of holding them for the whole session, so
  the board keeps far more of its scarce internal memory free at rest, well above the
  low-memory floor. Sound still plays and records the same; the parts come back the
  moment they are needed.
- **A deferred key check now explains itself and retries on its own.** When you
  paste a provider key and the device is low on working memory, it saves the key and
  shows "Verification deferred" instead of a failed check. The badge and a line under
  the key field now say what happened and what happens next: the key is saved, and it
  is checked again automatically when memory frees up, or you can tap Verify to retry.
  Hovering the badge shows the measured numbers (the largest free block and the amount
  the check needs). The device re-runs the check by itself on a short, backing-off
  schedule while memory is tight, and the Memory health row notes when a check has
  been waiting a while.
- **A Virtual Nimbus now accepts your own provider keys from its web app.** Setting
  an OpenAI, Anthropic, Mistral, or Z.ai key on the Providers & keys page of a hosted
  instance now takes effect on that instance, the same as on a device: the key shows
  "Key set", the model picker unlocks, turns run on that provider, and the key
  survives a restart. Before, only the Cumulo Nimbus key took; the other providers
  looked saved but nothing applied. Z.ai is now offered as a direct provider on a
  hosted instance too. An unrecognized key field is refused with a clear error rather
  than being quietly accepted.
- **A Virtual Nimbus reports its real version, like a device.** A hosted instance now
  shows the same engine version a physical device shows (for example v4.5.x) instead
  of a container tag, and each release builds and publishes the hosted image so an
  instance is never left several versions behind.
- **Checking for updates over a remote connection no longer shows a gateway
  error when the check actually worked.** The device accepts an update check right
  away and runs the download-and-verify work in the background, so a check started
  through the remote link returns promptly instead of the connection timing out at
  the edge while the device quietly finished. The result still appears on the
  Software update panel once it settles: up to date, a new version, or an honest
  failure with the reason.
- **Cumulo Nimbus is now a selectable voice provider.** The web Voice settings and
  the device Sound menu both offer Cumulo Nimbus for dictation and spoken replies,
  alongside Mistral and OpenAI. Choosing it routes voice through the metered router,
  so a one-key device can speak without a separate OpenAI or Mistral key. An explicit
  choice of Cumulo Nimbus is honored even when another provider key is present, and
  the voice picker shows an honest hint when no Cumulo key is set.
- **Memory tools say why when embeddings are unavailable, and search still
  answers.** When the assistant cannot turn text into a vector, it now names the
  real cause (no embeddings key, a rejected key, a busy connection, an
  unreachable provider, a timeout, an HTTP error, or a provider that is out of
  credit or rate limited) instead of one fixed "provider offline?" message, and
  never shows a key. Memory search and archive search keep working through the
  outage by falling back to a labeled keyword match over your stored memories, so
  recall no longer reads as empty. Saving a memory during an outage is refused
  with the real reason rather than stored without a vector.
- **A new Safety activity surface shows what the scanner blocked or flagged.** Under
  Assistant, Safety, Activity you see a list of the most recent blocked or suspected
  items, newest first. From it you can approve an item to allow that sender, that exact
  content, or that pattern from then on, dismiss it, or report it to Cumulo if you
  subscribe. Approving is always scoped and bound to the gate it came from, never a
  blanket off switch, and the Allowed items list below lets you revoke any approval.
- **The LAN MCP endpoint no longer buffers a request before checking the device
  token.** An unauthenticated `POST /mcp` is refused with nothing kept in memory, and
  an authenticated request larger than 64 KB is refused rather than silently cut
  short. Normal MCP clients see no change.
- **Power off and wake, and button feedback, are now checked repeatedly on real
  hardware.** A repeated hardware test drives many power-off then wake cycles and confirms the
  device comes back each time with its settings intact, and a second hardware check confirms
  every actionable menu control gives its sound and ring cue and then the ring goes
  dark. No change to how the device behaves; these close gaps in what was proven.

## v4.5.4 - Touch works again on classic boards, honest display check, cloud-sync headroom

- **Touch responds again on the classic (display + ring) board.** v4.5.0 introduced a
  background display-health poll that shares the display bus with the resistive
  touch controller on the classic board and could pin touch. That poll is now
  off on those boards; the display and touch reads the board shipped with in
  v4.4.6 are back as they were.
- **The display check tells the truth on the classic board.** Because that
  board cannot read its display controller without disturbing touch, the
  installer, the boot log, the health panel and the device status now say the
  display "cannot self-check on this board, look at the screen" instead of
  reporting it healthy. A display that fails to start is still reported as
  such. The Freenove board keeps its full self-check.
- **Cloud sync no longer runs out of memory.** The relay staging buffers behind
  "Not enough memory right now" live in the large PSRAM, and the memory floor
  the cloud dial checks is sized for that; a serve of the full device page now
  leaves the scarce internal memory almost untouched. A release check keeps
  those buffers from creeping back.
- **An unseeded Freenove is offered no update rather than the wrong size.** The
  three Freenove screen sizes cannot be told apart by the hardware, so a board
  with no seeded type no longer guesses 2.8" and is skipped by typed updates
  until it is set up with the installer.
- Installer and test tooling: `tools/board_info.py` reads a board over USB
  without resetting it; the installer picks the provisioning image by board and
  cable; the on-device test suite measures the classic board honestly instead
  of skipping.

## v4.4.9 to v4.5.3 - stability under low memory, honest routines and storage

These releases shipped without separate notes. The user-visible changes across them:

- Turns wait honestly under low memory instead of failing over to a different
  provider, and routines that could not run say why.
- A message the device already handled is never replayed after a restart, and
  rapid messages are batched into one turn.
- A durable error log on the card or flash, readable from the device page.
- A loud "SD not detected, memories are on the card" banner when the card
  drops, instead of an empty Memory panel.
- A first-class memory restore over HTTP from a device backup.
- The Cloud access card can mint a Cumulo key for the device.

## v4.4.8 - GPT-6 Astra, current model lineups, router-driven Cumulo catalog

- **The Cumulo Nimbus model list now comes from the router itself.** The device
  reads the router's routable catalog (models with a confirmed price, across
  every upstream), so the Cumulo dropdown offers exactly what your Cumulo key
  can route, newest flagships first, including Anthropic and Mistral models
  through the one key. Size classes ride along from the router.
- **Current models across every provider.** The built-in model lists now lead
  with each provider's September 2026 lineup: OpenAI `gpt-6-astra` plus the
  `gpt-5.6` Sol / Terra / Luna tiers, Anthropic `claude-opus-5` /
  `claude-sonnet-5` / `claude-haiku-4-5`, Z.ai `glm-5.3` / `glm-5.2` /
  `glm-5.3-flash`. Defaults move up with them (OpenAI `gpt-5.6`, Anthropic
  `claude-sonnet-5`, Z.ai `glm-5.3`, Cumulo Nimbus `gpt-5.6`), and the 5.6
  tier names now carry their size, so fallback rules by size match them. The
  Cumulo Nimbus model list also lists newest models first instead of oldest.
- **GPT-6 Astra can be selected.** OpenAI's `gpt-6-astra` (released 2026-09-03) is
  offered as the Orchestrator model and as the sub-session model under Assistant >
  Models, with its full context window and reasoning support. The OpenAI model list
  now keeps the newest GPT generation first instead of only the GPT-5 family, so
  the next generation shows up on its own. Cumulo Nimbus prices it from day one.
- **Cumulo Nimbus provider slot in the device Models UI.** Assistant > Models >
  Providers & keys now lists **Cumulo Nimbus** first, the recommended one-key path:
  paste your device key, and Verify checks it against the router (not a third-party
  API) with the base wiring already set, so there is no URL to type. **Z.ai** is
  listed too. Pasting a Cumulo key into a direct provider now says to use the Cumulo
  Nimbus slot instead of a bare "key rejected."
- Removed the deprecated e-ink display and rotary-knob code paths. Both shipping
  configurations are color touchscreens (the Solide S3 with its LED ring, and the
  Freenove CYD all-in-one). E-ink devices stay on their final firmware (typed OTA
  delivers them no further updates).
- **Multi-resolution touch UI.** The color renderer is now parameterized by screen
  resolution, so one code path draws every supported size. The 2.8" screen is
  unchanged (its goldens are byte-identical). Two larger Freenove sizes are added
  as their own firmware images and typed updates: **3.5" (480×320, `freenove-35`)**
  and **4.0" (480×480, `freenove-40`)**. Both are **host-verified only** - proven
  by per-size golden framebuffers and a linking firmware image, not yet run on a
  physical screen (the display driver gains the larger geometry when hardware is in
  hand). See [Supported panels](hardware/all-in-one-cyd.md#supported-panels).

## v4.1.0 - Orchestrator self-knowledge, honest capacity, real file delivery

Everything the assistant tells you about itself is now generated from live,
validated device state - never hardcoded prose - and it can finally produce and
deliver real files. Every change below traces to a two-sub-agent task (collect
news, build a PDF, deliver it over Telegram, then draft an email) that failed end
to end on v4.0.0, or to the review of the composed prompt that followed.

### 1. Dynamic, validated self-knowledge (the composed prompt)
- The provider/connector catalog is **generated from the live device state**:
  each provider is marked *available, VERIFIED* / *key present, not yet
  verified* / *REJECTED* from the real verify cache - never a checkbox printed
  as fact. Custom MCP servers get an honest "capabilities not catalogued" line.
- A new **[SUB-AGENT CAPABILITIES]** block states, per keyed provider, what a
  spawned agent actually gets - and the universal rule the failed task broke:
  **sub-agents gather (and may run their provider's connectors server-side);
  the head does every owner-delivery step.** It also answers "can a sub-agent
  code?" (anthropic sandbox: writes AND runs code; openai/mistral: Python via
  code_interpreter; nothing pushes to a repository).
- **Every prompt surface reads live state**: the role line carries the device's
  own name (a renamed device no longer introduces itself twice under two
  names); the identity block names *who this turn's message is from* and their
  role (multi-tenant honesty - not everyone is "your owner"); the hardware
  manifest reflects the fitted display, the live SD tier, the real battery
  hardware, and the actual LED count.
- **Per-connector credential state**: the catalog marks a failed OAuth sign-in
  ("sign-in FAILED - tell the owner") and a missing credential ("NO credential
  - not usable"), fed from the one credential choke point. Enabled is a
  checkbox; these markers are evidence.
- **Capability validation is a user setting** (now under Assistant > Tools >
  Tool use): Off / Passive (default) / Active. Passive marks a provider verified
  when a real turn succeeds; active adds a periodic free re-verify
  (`GET /v1/models`, ~$0) so a revoked key is caught.

### 2. On-board documentation (`docs.list` / `docs.search` / `docs.read`)
The device carries a generated pack of its own documentation (14 curated docs,
169 sections, ~195 KB of flash rodata) and the prompt tells the model to check
it before claiming what it can or cannot do. The pack is regenerated from
`docs/` at build time and ships inside `firmware.bin` - **OTA updates the
device's self-knowledge automatically and it can never version-skew.**

### 3. Complete live self-state (`device.status`)
One tool call now answers every "what is your current state" question: firmware
+ update engine + slot + auto-update, time + timezone + clock sync, heap/PSRAM,
SD (including live *lost* state) + internal flash, battery, ring posture +
battery mode (user and effective), memory counts **vs live caps** (vectors,
episodic, scratchpad), file-store usage + free, **sub-agent capacity**
(running/queued vs limits), real billed token usage, and every settable knob's
current value. `tenant.list` shows usage beside quota; `files.list` leads with
totals; the scratchpad view shows fill vs caps.

### 4. Scratchpad as a first-class response field
The model's persistent working memory (`active` / `short` / `mid` / `long`) is
now a top-level `orch_turn` response field - a free write with no tool round -
that survives across turns and restarts. The prompt renders it every turn with
the write instruction beside the data. It was an admin-only tool nobody
triggered; now multi-step plans persist. (Admin-gated: the global scratchpad
renders into every prompt, so non-admin writes are refused.)

### 5. Honest throughput - `[SPAWN CAPACITY]` (no concurrency, by design)
"6 spawn slots free" implied a hard ceiling; the model under-spawned deep runs.
The prompt now states the true model: N startable this turn, running
**sequentially** (single task, one TLS connection at a time), draining over
successive waves - *not a hard total limit*. A deeper pending queue (12,
decoupled from the concurrency window) accepts a full wave up front. A >60 s
Mistral sub now reports an honest timeout instead of the false "couldn't
start". Sub-agent concurrency was investigated and permanently rejected: it is
unstable on this hardware (see the development guide).

### 6. Provider file delivery - the PDF path
A sub-agent that generates a file in its provider sandbox now has that file
**captured to the SD artifact store and delivered over Telegram**:
- **OpenAI** code_interpreter (container files) - **including PDF** (verified
  live end-to-end at the API level).
- **Mistral** code_interpreter (files API) - images/text (their sandbox
  currently errors on PDF/CSV output; the capture is type-agnostic and needs
  no change when they fix it).
Downloads stream to SD under the TLS arbiter (the OTA pattern), are
Content-Length-verified (a truncated download is refused, never registered as
saved), and respect the file store's ownership boundary.

### 7. Ring: the orchestrator arc persists while children run
The head's blue arc used to collapse the instant a fan-out turn returned. A
host-tested reconciler on the always-alive main loop keeps it lit while
sub-agents run, bridges the synchronous-dispatch window, clears exactly once
when the last child finishes, and latches off if the system wedges (a dead
task can't pulse "working" forever).

### 8. Clock & timezone, dream honesty, memory dashboard (merged sibling work)
Owner-set timezone with device clock + sync badge on the web UI; routines adopt
a timezone edit budget-neutrally; the nightly dream skips its paid reflection
turn on quiet nights and respects an honest fact quota; the memory dashboard
gained lifecycle columns (created / expires-in / last-used), browse pagination,
Delete All (typed confirm), file content search, SD erase danger controls, and
photo-aware `files.send`.

### Main architectural decisions

1. **No on-device sub-agent concurrency, permanently.** Investigated on
   request; hardware evidence (the tlsSlots=2 A/B failure, recurring
   poll-task stack overflows, a watchdog reset from contending work, ~22.5 KB
   largest contiguous internal SRAM) closed it. Serial single-task/single-TLS
   is the design, not a limitation to fix. Throughput improves only via honest
   wording, queue depth, and remote-side parallelism (openai/anthropic).
2. **Self-knowledge is generated, never asserted.** Anything the prompt claims
   must come from a live seam (verify cache, HAL health, storage tier, tenant
   table, driver constants) - the same "advertised == callable" principle
   extended to "described == measured". Golden tests pin every prompt surface.
3. **Docs ride the firmware image.** Embedded generated pack (rodata) over
   LittleFS images or SD sync: zero RAM cost, works with no card, and OTA can
   never ship firmware whose self-description lags its behavior.
4. **Delivery is the head's job; provider sandboxes are hands.** Sub-agents
   return text plus captured file references; only the head touches the owner
   (Telegram/files/memory). This is the decomposition contract the prompt now
   teaches, and the file-capture path enforces it mechanically.
5. **Honesty gates at the data path.** Truncated downloads refuse to register;
   cross-tenant overwrites refuse at begin AND at the destructive rename; a
   transient verify failure cannot demote a proven credential; every "not
   captured / not usable / sign-in failed" state is surfaced instead of
   silently dropped.

### Verification
Host: 1164 native test cases green (grew from 1117 at v4.0.0). Device:
`esp32s3` + `test` builds green (~60% app slot after the docs pack). The full
diff was reviewed for regressions before tagging. Website docs regenerated and
building clean.

## v4.0.0 - Skills authoring · Deep research · Canonical wire · Deep history

### New
- **Skills authoring**: the assistant can draft its own reusable skills
  (`skill.save`/`skill.delete`). Every assistant-written skill arrives pending
  your approval (web Assistant → Skills, or `/skill approve <id>` in
  Telegram) and does nothing until approved. Built-in skills can't be
  overwritten; you can delete any skill, the assistant only its own.
- **Deep research** (built-in skill): multi-wave sub-agent fan-out into a
  per-run project on the SD card - each sub-agent's findings auto-save as a
  document, a final large-model writer gets the strongest docs attached, and
  the report is delivered to your Telegram.
- **Sub-agent attachments & projects**: a spawned sub-agent can carry up to 4
  on-device documents (spliced in by the firmware) and a project tag that
  auto-saves its full result - multi-stage runs plan from what actually landed.
- **Deep history**: conversation search now reaches months back - past what is
  loaded at startup - with honest paging ("searched back to a date; older
  history exists"). "I don't remember" now means it actually isn't there.
- **Mid-turn provider failover**: a turn that loses its provider partway
  through continues on the next verified provider, carrying its work
  (Settings toggle "Switch providers mid-turn", default on).

### Changed
- All three providers' tool loops now replay a single device-owned transcript
  (stateless wire) - eliminates the OpenAI chain-poisoning failure class.
- Prompt honesty: delivery claims ("sent", "spoken") require a tool result;
  memory saves are phrased as intent until confirmed; the spawn-slot budget is
  visible to the model so it says when it can't start everything.
- `/api/chat` `pending` now means a turn is actually running.

### Fixed
- Episodic search silently returned nothing from any day-file over 128 KB
  (tail-read offsets were indexed wrong) - a busy month of history was
  invisible while the RAM cache quietly served recent rows.
- Cold-history paging: byte cursors no longer re-emit recent rows or strand
  deep history on the shipped configuration.
- Attach reads now enforce the same per-person file boundary as `files.read`.
- Skill approval holds its lock across the read-modify-write, so a concurrent
  assistant save can't slip an unreviewed body into an approved skill.

## Older releases

Release history before v4.0.0 lives on the
[releases page](https://github.com/ristllin/nimbus-fw-releases/releases).
