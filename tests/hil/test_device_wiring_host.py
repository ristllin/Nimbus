"""Device wiring the host suite cannot run (no board).

The rules themselves are host tested in lib/core and lib/harness. What only a bench would
catch is a device call site losing its call, so each one is pinned here by reading the
source, the way test_action_rules pins the owner chat rules. A guard that cannot fail is
worse than none, so every guard takes the source as text and has a counter-test on the
broken form it exists to catch.
"""

from __future__ import annotations

import pathlib
import re

import pytest

pytestmark = pytest.mark.host

ROOT = pathlib.Path(__file__).resolve().parents[2]


def read(path: str) -> str:
    return (ROOT / path).read_text()


def body_of(source: str, signature: str) -> str:
    """The text of the function whose definition starts with `signature`, to its closing
    brace at column 0. Empty when it is gone, which fails the guard asking."""
    at = source.find(signature)
    if at < 0:
        return ""
    end = source.find("\n}\n", at)
    return source[at : end if end >= 0 else len(source)]


def before(text: str, first: str, second: str) -> bool:
    a, b = text.find(first), text.find(second)
    return a >= 0 and b >= 0 and a < b


# ---- approvals write the tenant row, judged from the lists just written ---------------


def approval_faults(telegram_cpp: str) -> list:
    body = body_of(telegram_cpp, "bool approvePending(const String& chatId")
    faults = []
    if "orchestrator::tenantApproved(" not in body:
        faults.append("an approval writes no tenant row")
    if not re.search(r"telegramOwnerOf\(std::string\(al\.c_str\(\)\)", body):
        faults.append("the owner rule does not read the allowlist just written")
    if re.search(r"\bisOwner\(", body) or "g_allowlist" in body or "g_owners" in body:
        faults.append("an approval reads the poll task's lists from the web task")
    return faults


def test_an_approval_writes_the_row_from_the_lists_it_just_wrote():
    assert approval_faults(read("src/agent/telegram.cpp")) == []


@pytest.mark.parametrize(
    "line,fault",
    [
        ("", "writes no tenant row"),
        ("agent::orchestrator::tenantApproved(id, isOwner(chatId));", "poll task"),
    ],
)
def test_the_approval_guard_sees_a_broken_call(line, fault):
    source = "bool approvePending(const String& chatId, const String& name) {\n  " + line + "\n}\n"
    assert any(fault in f for f in approval_faults(source))


# ---- files.read refuses binary content on both branches -------------------------------


def files_read_faults(files_cpp: str) -> list:
    faults = []
    tail = body_of(files_cpp, "static nimbus::orch::ToolResult readTail(")
    if "fileReadableAsText(name," not in tail or not before(tail, "fileReadableAsText(name,", "tailRows("):
        faults.append("files.read with tail shows a binary file")
    if "kNotTextRefusal" not in tail:
        faults.append("files.read with tail does not refuse in the shared words")
    at = files_cpp.find('reg.add("files.read"')
    whole = files_cpp[at : files_cpp.find("reg.add(", at + 10)] if at >= 0 else ""
    if not before(whole, "fileReadableAsText(name, text)", "utf8CapLen("):
        faults.append("files.read shows a binary file")
    return faults


def test_every_files_read_branch_asks_the_rule():
    assert files_read_faults(read("src/agent/files_subsystem.cpp")) == []


def test_the_files_read_guard_sees_the_tail_branch_skip_the_rule():
    source = (
        "static nimbus::orch::ToolResult readTail(int a) {\n  auto v = tailRows(w);\n}\n"
        'reg.add("files.read", [] { fileReadableAsText(name, text); utf8CapLen(p); });\nreg.add("x");\n'
    )
    assert files_read_faults(source)[0] == "files.read with tail shows a binary file"


# ---- image.look says when a reading is still waiting ----------------------------------


def look_faults(vision_cpp: str) -> list:
    faults = []
    caption = body_of(vision_cpp, "static std::string captionAnswer(")
    if "lookFailedWords(readingWaits(ref)" not in caption:
        faults.append("a failed caption of a queued photo reads as a plain failure")
    if len(re.findall(r"lookFailedWords\(readingWaits\(ref\)", vision_cpp)) < 2:
        faults.append("a failed fetch of a queued photo reads as a plain failure")
    return faults


def test_image_look_asks_whether_the_reading_waits():
    assert look_faults(read("src/agent/adapters/image_vision.cpp")) == []


def test_the_look_guard_sees_the_question_replaced():
    source = "static std::string captionAnswer(int) {\n  err = lookFailedWords(false, r.words);\n}\n"
    assert look_faults(source)


# ---- a copy this sender keeps is not charged twice ------------------------------------


def dedupe_faults(files_cpp: str, telegram_cpp: str) -> list:
    faults = []
    save = body_of(files_cpp, "bool saveStream(const std::string& project")
    if not before(save, "g_store.sameContent(project, hash, ownerNs)", "staged.fs.open(") or not re.search(
        r"sameContent\(project, hash, ownerNs\)\)\s*\{.*?return true;", save, re.S
    ):
        faults.append("saveStream stores a second copy")
    refusal = body_of(telegram_cpp, "static const char* refusalAfterFetch(")
    if not before(refusal, "files::holdsSameContent(", "files::wouldExceedQuota("):
        faults.append("the check after the download charges a copy")
    holds = body_of(files_cpp, "bool holdsSameContent(")
    if "g_store.sameContent(" not in holds or "!= nullptr" not in holds:
        faults.append("holdsSameContent does not ask the store")
    return faults


def test_a_received_copy_is_not_charged_twice():
    assert dedupe_faults(read("src/agent/files_subsystem.cpp"), read("src/agent/telegram.cpp")) == []


def test_the_dedupe_guard_sees_each_revert():
    good_files = (
        "bool saveStream(const std::string& project) {\n"
        "  if (g_store.sameContent(project, hash, ownerNs)) {\n    return true;\n  }\n"
        "  File src = staged.fs.open(p);\n}\n"
        "bool holdsSameContent(int) {\n  return g_store.sameContent(a, b, c) != nullptr;\n}\n"
    )
    good_tg = (
        "static const char* refusalAfterFetch(int) {\n"
        "  if (files::holdsSameContent(a) || !files::wouldExceedQuota(w, t)) return nullptr;\n}\n"
    )
    assert dedupe_faults(good_files, good_tg) == []
    no_skip = good_files.replace("  if (g_store.sameContent(project, hash, ownerNs)) {\n    return true;\n  }\n", "")
    assert dedupe_faults(no_skip, good_tg) == ["saveStream stores a second copy"]
    charged = good_tg.replace("files::holdsSameContent(a) || ", "")
    assert dedupe_faults(good_files, charged) == ["the check after the download charges a copy"]


# ---- a spend ceiling under the floor is refused on every owner surface ----------------


def ceiling_faults(webui_cpp: str, console_cpp: str) -> list:
    faults = []
    at = webui_cpp.find('if (n == "spendCeil") {')
    branch = webui_cpp[at : webui_cpp.find('if (n == "dailyWarn")', at)] if at >= 0 else ""
    if not re.search(r"micro == 0\) \{ err = nimbus::orch::kCeilingFieldRefusal;", branch):
        faults.append("the device web page does not refuse a low ceiling")
    if re.search(r"parseCeilingCents\([^;]*,\s*\w+", branch):
        faults.append("the device web page parses the ceiling with another floor")
    if not re.search(r"parseCeilingCents\(std::string\(c\.c_str\(\)\)\);\s*if \(micro == 0\)", console_cpp):
        faults.append("the test console does not refuse a low ceiling")
    return faults


def test_every_ceiling_surface_refuses_under_the_floor():
    assert ceiling_faults(read("src/net/webui.cpp"), read("src/test_console.cpp")) == []


def test_the_ceiling_guard_sees_a_floor_passed_in():
    web = (
        'if (n == "spendCeil") {\n  const uint64_t micro = parseCeilingCents(v, 10000ULL);\n'
        "  if (micro == 0) { err = nimbus::orch::kCeilingFieldRefusal; return true; }\n}\n"
        'if (n == "dailyWarn") {}\n'
    )
    console = "const uint64_t micro = parseCeilingCents(std::string(c.c_str()));\n    if (micro == 0) {}\n"
    assert ceiling_faults(web, console) == ["the device web page parses the ceiling with another floor"]


# ---- no provider-bound builder writes a raw control byte ------------------------------
#
# ArduinoJson's serializeJson leaves 26 control bytes raw, and a strict JSON parser refuses
# the whole request (nimbus/json_strict.h). Every builder of a provider request uses the
# strict writer. The two lists below are the class: a new adapter lands in the glob.
PROVIDER_BUILDERS = (
    "lib/core/include/nimbus/orch/image_gen.h",
    "lib/core/src/orch_embedding.cpp",
    "lib/harness/src/vision_caption.cpp",
    "lib/harness/src/websearch.cpp",
)
# A bare serialize that does not build a request, by file: an upstream's error kept for the log.
NOT_A_REQUEST = {"src/agent/adapters/url_fetch.cpp": {"serializeJson(resp, said)"}}


def bare_serializes(source: str) -> set:
    calls = re.findall(r"(?<![\w.])(?:ArduinoJson::)?serializeJson\([^;]*?\)(?=;)", source)
    return {c.replace("ArduinoJson::", "") for c in calls}


def raw_builders(root: pathlib.Path) -> dict:
    files = [root / p for p in PROVIDER_BUILDERS]
    files += sorted((root / "src/agent/adapters").glob("*.cpp"))
    found = {}
    for path in files:
        if not path.is_file():
            found[str(path)] = {"the file is gone: update the guard"}
            continue
        rel = path.relative_to(root).as_posix()
        extra = bare_serializes(path.read_text()) - NOT_A_REQUEST.get(rel, set())
        if extra:
            found[rel] = extra
    return found


def test_every_provider_builder_writes_strict_json():
    assert raw_builders(ROOT) == {}


@pytest.mark.parametrize(
    "adapter",
    ["moderation.cpp", "audio_tts.cpp", "url_fetch.cpp", "image_vision.cpp"],
)
def test_the_device_adapters_are_in_the_class(adapter):
    assert (ROOT / "src/agent/adapters" / adapter).is_file()


def test_the_strict_writer_is_used_where_it_is_claimed():
    for adapter in ("moderation.cpp", "audio_tts.cpp", "url_fetch.cpp"):
        assert "nimbus::serializeStrictJson(" in read(f"src/agent/adapters/{adapter}"), adapter


def test_the_builder_guard_sees_a_new_adapter_and_a_revert(tmp_path):
    for p in PROVIDER_BUILDERS:
        (tmp_path / p).parent.mkdir(parents=True, exist_ok=True)
        (tmp_path / p).write_text("nimbus::serializeStrictJson(d, out);\n")
    adapters = tmp_path / "src/agent/adapters"
    adapters.mkdir(parents=True)
    (adapters / "url_fetch.cpp").write_text("String b; serializeJson(resp, said);\n")
    assert raw_builders(tmp_path) == {}
    (adapters / "new_thing.cpp").write_text("String body; serializeJson(d, body);\n")
    (tmp_path / PROVIDER_BUILDERS[0]).write_text("ArduinoJson::serializeJson(doc, out);\n")
    assert raw_builders(tmp_path) == {
        "src/agent/adapters/new_thing.cpp": {"serializeJson(d, body)"},
        PROVIDER_BUILDERS[0]: {"serializeJson(doc, out)"},
    }
