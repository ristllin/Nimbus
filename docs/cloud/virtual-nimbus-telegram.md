<!-- audience: user -->
# Telegram on a Virtual Nimbus

A Virtual Nimbus (a hosted instance) answers on Telegram the same way the desk
device does: **it trusts nobody until you approve them.** This page covers
connecting a bot, approving yourself, and deciding who else may use it.

## Connect a bot

1. In Telegram, message **@BotFather**, create a bot, and copy its token.
2. When you create the instance at
   [app.cumulo-nimbus.ai](https://app.cumulo-nimbus.ai), choose **Connect
   Telegram (optional)** and paste the token.

The bot is now live, but it answers no one yet.

## Approve yourself

1. Message your bot once from your own Telegram account. It replies that it only
   answers people its owner has approved, and tells you your chat ID.
2. Open your instance's web app and go to **Assistant**, **Connectors**,
   **Telegram**. Your message is waiting under **Who can message this device**.
3. Tap **Approve**. Your next message gets a real answer.
4. Make your own chat an **admin**: click the role on your chip until it shows
   **admin**. As an admin, your Telegram chat shares the memories and files of your
   web chat; until then it is a separate **user**.

If you already know a chat ID, type it into **Chat ID** and tap **Add** instead of
waiting for a message.

## Let other people in

Anyone else who messages the bot gets the same polite refusal and waits in the
same list for you. They are never served until you approve them, and a stranger
who keeps writing is not sent the refusal more than once every 10 minutes.

Everyone you approve starts as a **user**; nobody becomes an admin unless you make
them one. Each person's role is shown on their chip. Click it to cycle **admin**,
**user**, **guest**:

| Role | What it means on a Virtual Nimbus |
|---|---|
| **Admin** | The owner: your memories, your files, and every tool. |
| **User** | Their own memories and history, kept apart from yours. |
| **Guest** | Like a user, with tighter storage limits and memories that expire sooner. |

Things to know before you approve someone:

- **An approved person's conversations use your connectors** (calendar, Notion,
  Slack, and the rest) and your provider keys, and they can start background
  agents that do the same. Approving someone is granting that.
- **One person's background agents at a time.** While your agents are working,
  a user's or guest's message waits until your report is delivered, so they never
  see your results; an agent they ask for while someone else's are running is
  declined with a short note. Your own messages never wait.
- **Stored files stay yours.** On a hosted instance a user or guest cannot list,
  read, or save files.
- **Removing someone** (the × on their chip) takes effect on their next message,
  and their role goes with them: if you approve them again later, they start as a
  user.
- **There is no open access.** The device's **Open access** switch is refused on a
  hosted instance: a public bot would spend your keys and reach your connectors.

The list and the roles are stored on the instance, so they survive restarts and
updates. Roles, limits, and privacy work as described in
[People and privacy](../people-and-privacy.md).

## For operators: `NIMBUSD_TG_CHAT_ID`

Older instances were locked to one chat with the `NIMBUSD_TG_CHAT_ID`
environment variable. It is now **legacy and optional**. If it is set, it adds
that chat to an EMPTY allowlist once, as the admin, and is then used up: it never
changes a list the owner already manages, and a chat the owner removes does not
come back on restart. With it unset, the instance starts with nobody approved.
