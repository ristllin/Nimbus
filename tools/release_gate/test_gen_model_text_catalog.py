"""Host tests for the model-text catalog generator (gen_model_text_catalog.py).

Run: python3 -m pytest tools/release_gate

The lexer reads C and C++ string literals the way the compiler joins them; the
catalog keeps every model-facing literal and drops log lines; the golden on disk is
what the sources produce now.
"""

import os

import pytest

import gen_model_text_catalog as cat

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))


def lits(src):
    return [lit.text for lit in cat.extract_literals(src)]


def test_adjacent_literals_join_across_lines_comments_and_macro_continuations():
    src = '#define D \\\n  "one " \\\n  "two"\nconst char* x = "a " /* c */ "b " // tail\n  "c";\n'
    assert lits(src) == ["one two", "a b c"]


def test_raw_strings_escapes_and_utf8_bytes():
    src = 'auto a = R"x(raw "quoted" \\n)x"; auto b = "tab\\there\\n"; auto c = "\\xe2\\x80\\x94 dash"; auto d = u8"caf\\u00e9";'
    assert lits(src) == ['raw "quoted" \\n', "tab\there\n", chr(0x2014) + " dash", "café"]


def test_char_literals_comments_and_includes_are_not_strings():
    src = '#include "nimbus/x.h"\nchar q = \'"\'; // "not a string"\n/* "nor this" */ auto s = "real one";\n'
    assert lits(src) == ["real one"]


def test_log_calls_are_marked_and_nested_calls_are_seen():
    src = (
        'void f() { hlog::logf("a log %d", 1); Serial.printf("serial %s", x); alogf("dev %d", 2);\n'
        'return ToolResult::fail(std::string("not saved: ") + why); }'
    )
    out = cat.extract_literals(src)
    assert [lit.logged for lit in out] == [True, True, True, False]
    assert out[-1].calls[-2:] == ("fail", "string")


def test_registration_arguments_and_named_descriptions():
    src = (
        'void r(Reg& reg) {\n reg.add("files.read", std::string("Read a file by name ") + x,\n'
        '  [](Args a) { return ToolResult::ok("read it all"); }, R"({"type":"object"})");\n'
        ' addOwnerOnly(reg, "files.append", "Add rows to the end", h);\n}\n'
        'constexpr char kAppendDesc[] = "described here";\n'
    )
    out = {lit.text: lit for lit in cat.extract_literals(src)}
    assert out["Read a file by name "].in_add
    assert not out["read it all"].in_add
    assert out['{"type":"object"}'].in_add
    assert out["Add rows to the end"].in_add
    assert "kAppendDesc" in out["described here"].names


def test_namespaces_are_recorded():
    src = 'namespace a { namespace core { constexpr char k[] = "hi there"; } }\nconst char* z = "out side";'
    out = cat.extract_literals(src)
    assert out[0].namespaces == ("a", "core")
    assert out[1].namespaces == ()


@pytest.mark.parametrize(
    "text, prose",
    [
        ("no open items", True),
        ("files.read", False),
        ("%s: %d", False),
        ("/mem/tx", False),
        (" B", False),
        ("a b", False),
        ("ok go", True),
        ("Never\nretry", True),
        ("Never\u00a0retry", True),
        ("%s\n", False),
    ],
)
def test_prose(text, prose):
    assert cat.is_prose(text) is prose


def test_each_literal_is_classed_by_what_it_is_part_of():
    src = (
        'void reg(R& r) { r.add("x.y", "Describe the tool here", [](A a) {\n'
        '  hlog::logf("x.y called %d", 1);\n  return ToolResult::fail(describe("the file is missing")); }); }\n'
        'const char* page = "a page for the owner";\n'
        'constexpr char kListDesc[] = "List the things";\n'
    )
    out = cat._e1_file_entries("device_notes", "f.cpp", src)
    assert [(e.cls, e.text) for e in out] == [
        ("tool_descriptions", "Describe the tool here"),
        ("tool_results", "the file is missing"),
        ("device_notes", "a page for the owner"),
        ("tool_descriptions", "List the things"),
    ]


def test_digit_separators_and_prefixed_chars_do_not_hide_literals():
    src = "int n = 1'000; f(\"you must retry now\"); int m = 0xFF'FF; char c = L'\"'; auto s = \"after it\";"
    assert lits(src) == ["you must retry now", "after it"]


def test_a_backslash_newline_inside_a_literal_is_dropped():
    assert lits('auto s = "abc\\\ndef";') == ["abcdef"]


def test_an_unterminated_literal_fails_with_the_marker():
    with pytest.raises(cat.CatalogError):
        cat.model_literals("x.cpp", 'auto s = "never closed;')


def test_render_parse_round_trip():
    entries = cat.build_e1(REPO)
    text = cat.render("e1", entries)
    back = cat.parse(text)
    assert back["entries"] == sorted(entries, key=lambda e: (cat.E1_CLASSES.index(e.cls), e.path))
    assert back["classes"]["skills"]["ceiling"] == cat.E1_CEILINGS["skills"]
    assert cat.render("e1", back["entries"]) == text


def test_class_identity_ignores_the_file_a_text_sits_in():
    a = [cat.Entry("system", "a.cpp", "one text"), cat.Entry("system", "b.cpp", "two text")]
    b = [cat.Entry("system", "c.cpp", "two text"), cat.Entry("system", "a.cpp", "one text")]
    assert cat.class_stats("e1", a)["system"] == cat.class_stats("e1", b)["system"]


def test_every_listed_e1_source_exists_and_is_listed_once():
    rows = [p for _, p in cat.e1_sources()]
    assert len(rows) == len(set(rows))
    missing = [p for p in rows if not os.path.isfile(os.path.join(REPO, p))]
    assert missing == [], "an engine-1 source moved: update E1_SOURCES (" + ", ".join(missing) + ")"


def test_every_tool_file_is_catalogued():
    assert cat.uncatalogued_tool_files(REPO) == []


def test_every_e1_class_has_text_and_a_ceiling():
    stats = cat.class_stats("e1", cat.build_e1(REPO))
    for cls in cat.E1_CLASSES:
        assert stats[cls]["bytes"] > 0
        assert stats[cls]["bytes"] <= cat.E1_CEILINGS[cls]


def test_e2_ceilings_are_the_prd_figures():
    assert cat.E2_CEILINGS == {
        "core": 3072,
        "device_notes": 2048,
        "stubs": 512,
        "tool_results": 6144,
        "probe": 1024,
        "summarizer": 1024,
        "tool_descriptions": 10240,
    }


def test_the_goldens_are_fresh(capsys):
    assert cat.main(["--root", REPO]) == 0
    assert capsys.readouterr().out.strip().splitlines()[-1] == "MODEL TEXT CATALOG GREEN"
