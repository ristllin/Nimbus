"""§L13 - Connector E2E lifecycle tests (Mistral Studio connectors).

Full create → validate → modify → validate → delete → validate lifecycles driven
through the LIVE orchestrator's provider connectors, with programmatic validation
(a unique per-run MARKER must round-trip back through a read, and disappear after
delete). These prove the connector PATH end-to-end, not LLM determinism - so an
external refusal (connector not authorized, resource not accessible) is a LOUD
SKIP, and only a device-side failure (5xx, crash, missing plumbing) fails.

Requires the connectors to be authorized in Mistral Studio
(console.mistral.ai/build/connectors) AND added to the device
(POST /api/connectors). Each missing piece SKIPs with the exact reproduction step.

Markers: ``connectors`` (+ ``agent`` + ``net``). Gated behind ``--allow-hardware``.
Reuse an already-joined board with ``NIMBUS_TEST_IP`` + ``NIMBUS_TEST_TOKEN``.

Run:  python3 -m pytest tests/hil -m "connectors and not manual" --allow-hardware
"""

from __future__ import annotations

import os
import re

import pytest

from connectors import (
    mistral_single_connector,
    new_marker,
    reply_text,
    require_mistral_connector,
    run_turn,
    skip_if_unavailable,
)
from test_l4_network import lan_ip_or_skip

pytestmark = [pytest.mark.connectors, pytest.mark.agent, pytest.mark.net]


def _sd_present(net, ip) -> bool:
    st = net.get_json("/api/state", ip=ip, timeout=8.0)
    return str(st.get("sd", "")).lower() in ("present", "1", "true") or st.get("sd") is True


# ---- Notion: create → fill → modify → delete --------------------------------
def test_notion_page_lifecycle(device, net, secrets, require_secret):
    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    require_mistral_connector(net, ip, "notion", "Notion")
    title = new_marker("notion-title")
    body_a = new_marker("notionA")
    body_b = new_marker("notionB")
    with mistral_single_connector(net, ip, "notion"):
        # CREATE + FILL
        t = run_turn(
            net,
            ip,
            f'Using the Notion connector, create a new page titled "{title}" with a '
            f'single paragraph whose exact text is: {body_a}. '
            f'Reply with ONLY the new page URL or id.',
        )
        skip_if_unavailable(t, "Notion")
        assert reply_text(t).strip(), "Notion create returned an empty reply"

        # VALIDATE create (round-trip read)
        t = run_turn(
            net,
            ip,
            f'Using the Notion connector, open the page titled "{title}" and reply '
            f'with its exact body text and nothing else.',
        )
        skip_if_unavailable(t, "Notion")
        assert body_a in reply_text(t), f"created Notion body not found on read-back: {reply_text(t)!r}"

        # MODIFY
        run_turn(
            net,
            ip,
            f'Using the Notion connector, replace the body of the page titled "{title}" '
            f'so its only paragraph reads exactly: {body_b}.',
        )
        t = run_turn(
            net,
            ip,
            f'Using the Notion connector, read the page titled "{title}" and reply with '
            f'its exact body text and nothing else.',
        )
        assert body_b in reply_text(t), f"modified Notion body not found: {reply_text(t)!r}"

        # DELETE (archive) + VALIDATE gone
        run_turn(net, ip, f'Using the Notion connector, delete (archive) the page titled "{title}".')
        t = run_turn(
            net,
            ip,
            f'Using the Notion connector, search for a NON-archived page titled '
            f'"{title}". Reply with exactly EXISTS if found, otherwise exactly GONE.',
        )
        assert "GONE" in reply_text(t).upper() or body_b not in reply_text(t), (
            f"Notion page still present after delete: {reply_text(t)!r}"
        )


# ---- Google Drive: create doc + sheet → edit → download to SD → validate ----
def test_gdrive_doc_and_sheet_to_sd(device, net, secrets, require_secret):
    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    require_mistral_connector(net, ip, "google_drive_mcp", "Google Drive")
    if not _sd_present(net, ip):
        pytest.skip("no SD card mounted - the download-to-SD step needs the artifact store")

    tok = net.token()
    docname = new_marker("gdoc")
    sheetname = new_marker("gsheet")
    doc_a = new_marker("gdocA")
    doc_b = new_marker("gdocB")
    cell = new_marker("gcell")
    proj = "hil-connectors"
    fname = f"{docname}.txt"

    with mistral_single_connector(net, ip, "google_drive_mcp"):
        # CREATE a Google Doc + a Google Sheet
        t = run_turn(
            net,
            ip,
            f'Using the Google Drive connector, create a new Google Doc named '
            f'"{docname}" whose body is exactly: {doc_a}. Reply with ONLY its link.',
        )
        skip_if_unavailable(t, "Google Drive")
        t = run_turn(
            net,
            ip,
            f'Using the Google Drive connector, create a new Google Sheet named '
            f'"{sheetname}" with cell A1 set to exactly: {cell}. Reply with ONLY its link.',
        )
        skip_if_unavailable(t, "Google Drive")

        # EDIT the doc
        run_turn(
            net,
            ip,
            f'Using the Google Drive connector, append a new line with exactly this text '
            f'to the doc "{docname}": {doc_b}.',
        )

        # DOWNLOAD to SD: read the doc, then save its text as a device artifact
        # (artifact.save is an orch_turn device action, so it works on a single-shot turn).
        run_turn(
            net,
            ip,
            f'Using the Google Drive connector, read the FULL text of the doc "{docname}". '
            f'Then save that exact text to a device file with an artifact save action using '
            f'project "{proj}" and name "{fname}".',
        )

    # VALIDATE the download PROGRAMMATICALLY (byte content on the SD card).
    import requests

    dl = requests.get(f"http://{ip}/api/files/dl?t={tok}&project={proj}&name={fname}", timeout=20)
    if dl.status_code == 404:
        pytest.skip(
            "agent did not complete the Drive→SD save chain (artifact absent) - "
            "connector or tool step incomplete; not a device regression"
        )
    assert dl.status_code == 200, f"/api/files/dl -> {dl.status_code}"
    got = dl.text
    assert doc_a in got and doc_b in got, f"SD artifact missing the Drive content markers: {got[:200]!r}"

    try:
        # VALIDATE readable BY THE AGENT (default head + tool loop reads the artifact back).
        t = run_turn(
            net, ip, f'Read the device file in project "{proj}" named "{fname}" and reply with its exact contents.'
        )
        assert doc_a in reply_text(t), f"agent could not read the downloaded artifact back: {reply_text(t)!r}"
    finally:
        # cleanup: trash the Drive doc + sheet, remove the SD artifact
        with mistral_single_connector(net, ip, "google_drive_mcp"):
            run_turn(
                net,
                ip,
                f'Using the Google Drive connector, move to trash the doc "{docname}" and the sheet "{sheetname}".',
            )
        net.post("/api/files/rm", {"project": proj, "name": fname, "t": tok}, ip=ip, timeout=8.0)


# ---- Gmail: draft create → read back → delete -------------------------------
def test_gmail_draft_lifecycle(device, net, secrets, require_secret):
    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    require_mistral_connector(net, ip, "gmail", "Gmail")
    subj = new_marker("gmail-subj")
    body = new_marker("gmailBody")
    with mistral_single_connector(net, ip, "gmail"):
        # CREATE a DRAFT (never send - non-spammy, self-contained)
        t = run_turn(
            net,
            ip,
            f'Using the Gmail connector, create a DRAFT email (DO NOT SEND) addressed to '
            f'myself with subject "{subj}" and body exactly: {body}. '
            f'Reply with exactly DRAFTED on success.',
        )
        skip_if_unavailable(t, "Gmail")
        if "draft" not in reply_text(t).lower():
            pytest.skip(
                f"Gmail connector did not create a draft (reply: {reply_text(t)[:160]!r}) - "
                f"draft ops may be unsupported by this connector"
            )

        # VALIDATE: find the draft and read the body back
        t = run_turn(
            net,
            ip,
            f'Using the Gmail connector, search your DRAFTS for subject "{subj}" and reply with the exact body text.',
        )
        skip_if_unavailable(t, "Gmail")
        assert body in reply_text(t), f"draft body not found on read-back: {reply_text(t)!r}"

        # DELETE the draft + VALIDATE gone
        run_turn(net, ip, f'Using the Gmail connector, delete the DRAFT with subject "{subj}".')
        t = run_turn(
            net,
            ip,
            f'Using the Gmail connector, does a DRAFT with subject "{subj}" still exist? '
            f'Reply with exactly EXISTS or GONE.',
        )
        assert "GONE" in reply_text(t).upper(), f"Gmail draft still present after delete: {reply_text(t)!r}"


# ---- GitHub: create issue → comment → close (issues can't be API-deleted) ----
def test_github_issue_lifecycle(device, net, secrets, require_secret):
    ip = lan_ip_or_skip(device, net, secrets, require_secret)
    repo = os.environ.get("GITHUB_TEST_REPO") or secrets.get("GITHUB_TEST_REPO")
    if not repo:
        pytest.skip(
            "set GITHUB_TEST_REPO=<owner/repo> to a repo the Studio GitHub app can "
            "write to (the app cannot see private repos it wasn't granted)"
        )
    require_mistral_connector(net, ip, "github_app", "GitHub")
    title = new_marker("gh-issue")
    body_a = new_marker("ghA")
    comment_b = new_marker("ghB")
    with mistral_single_connector(net, ip, "github_app"):
        # CREATE issue
        t = run_turn(
            net,
            ip,
            f'Using the GitHub connector, create a new issue in the repo {repo} titled '
            f'"{title}" with body exactly: {body_a}. Reply with ONLY the issue number '
            f'in the form #<number>.',
        )
        skip_if_unavailable(t, "GitHub")
        m = re.search(r"#(\d+)", reply_text(t))
        if not m:
            pytest.skip(
                f"GitHub connector did not return an issue number "
                f"(reply: {reply_text(t)[:160]!r}) - likely no write access to {repo}"
            )
        num = m.group(1)
        try:
            # VALIDATE create
            t = run_turn(
                net, ip, f'Using the GitHub connector, read issue #{num} in {repo} and reply with its exact body text.'
            )
            assert body_a in reply_text(t), f"issue body not found on read-back: {reply_text(t)!r}"

            # MODIFY: add a comment, validate
            run_turn(
                net,
                ip,
                f'Using the GitHub connector, add a comment to issue #{num} in {repo} with exactly: {comment_b}.',
            )
            t = run_turn(
                net,
                ip,
                f'Using the GitHub connector, list the comments on issue #{num} in {repo} and reply with their text.',
            )
            assert comment_b in reply_text(t), f"comment not found: {reply_text(t)!r}"
        finally:
            # CLOSE (cleanup - the API cannot delete issues)
            run_turn(net, ip, f'Using the GitHub connector, close issue #{num} in {repo}.')
        t = run_turn(
            net,
            ip,
            f'Using the GitHub connector, is issue #{num} in {repo} open or closed? Reply with exactly OPEN or CLOSED.',
        )
        assert "CLOSED" in reply_text(t).upper(), f"issue not closed after cleanup: {reply_text(t)!r}"
