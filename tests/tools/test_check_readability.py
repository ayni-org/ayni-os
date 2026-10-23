"""Readability check scenarios using temporary repository trees, not Core builds."""

import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import check_readability as check  # noqa: E402


def function(name="run", body_lines=79):
    return f"void {name}() {{\n" + "    work();\n" * body_lines + "}\n"


class Readability(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        for directory in ("core", "include/ayni", "tools"):
            (self.root / directory).mkdir(parents=True)
        self.baseline = self.root / "tools/readability_baseline.json"
        self.baseline.write_text(json.dumps(dict(version=1, violations=[], exceptions=[])))

    def write(self, name, text):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
        return path

    def findings(self, rule):
        return [finding for finding in check.inspect(self.root) if finding.rule == rule]

    def run_check(self, *args):
        output, errors = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(errors):
            result = check.main(["--root", str(self.root), *args])
        return result, output.getvalue(), errors.getvalue()

    def record_baseline(self):
        self.assertEqual(self.run_check("--write-baseline")[0], 0)

    def test_exact_function_limit_passes_and_one_more_line_fails(self):
        self.write("core/work.cpp", function(body_lines=78))
        self.assertEqual(self.run_check()[0], 0)
        self.write("core/work.cpp", function())
        result, _, errors = self.run_check()
        self.assertEqual(result, 1)
        self.assertIn("function has 81 lines", errors)

    def test_multiline_signature_counts_from_its_first_line(self):
        self.write("core/work.cpp", "bool\nrun(\nint item)\n{\n" + "    work();\n" * 76 + "}\n")
        finding, = self.findings("function-lines")
        self.assertEqual(finding.line, 1)
        self.assertEqual(finding.value, 81)

    def test_nested_braces_lambda_comments_and_literals_do_not_end_function(self):
        body = 'if (ready) { auto callback = [] { return "}"; }; }\n'
        body += '// } fake() {\nconst char *text = R"tag({ /* not a comment */ })tag";\n'
        self.write("core/work.cpp", "void run() {\n" + body + "work();\n" * 76 + "}\n")
        finding, = self.findings("function-lines")
        self.assertEqual(finding.value, 81)

    def test_template_inline_member_and_constructor_initializers(self):
        text = "namespace outer {\ntemplate<class T> struct Value {\npublic:\n"
        text += "Value() : first{0}, second(1) {\n" + "work();\n" * 79 + "}\n"
        text += function("run") + "};\n}\n"
        self.write("core/work.h", text)
        found = self.findings("function-lines")
        self.assertEqual(len(found), 2)
        self.assertTrue(all("namespace outer" in item.subject for item in found))
        self.assertTrue(any("Value ( )" in item.subject for item in found))

    def test_overloads_have_distinct_baseline_identities(self):
        self.write("core/work.cpp", function("run") + function("run", 80).replace("run()", "run(int x)"))
        self.record_baseline()
        self.assertEqual(len(self.findings("function-lines")), 2)

    def test_default_template_arguments_and_specializations_are_definitions(self):
        definitions = ["template <typename T = int> void run()",
                       "template <int N = (2 > 1)> void run()",
                       "template <> void run<int>()",
                       "template <class T> bool operator<(T left, T right)"]
        for declaration in definitions:
            with self.subTest(declaration=declaration):
                self.write("core/work.h", declaration + " {\n" + "work();\n" * 79 + "}\n")
                finding, = self.findings("function-lines")
                self.assertEqual(finding.value, 81)

    def test_ambiguous_template_expressions_fail_explicitly_instead_of_skipping_functions(self):
        for declaration in ("template <int N = 1 << 2> void run()",
                            "template <bool B = 1 < 2> void run()"):
            with self.subTest(declaration=declaration):
                self.write("core/work.h", declaration + " {\n" + "work();\n" * 79 + "}\n")
                result, _, errors = self.run_check()
                self.assertEqual(result, 2)
                self.assertIn("template delimiters", errors)

    def test_attribute_operator_and_trailing_return_definitions(self):
        definitions = ["[[nodiscard]] auto run() const -> bool",
                       "[[nodiscard(\"message\")]] bool test()",
                       "bool operator()(int value) const",
                       "int operator[](int index) const", "explicit operator bool() const"]
        for declaration in definitions:
            with self.subTest(declaration=declaration):
                self.write("core/work.h", "struct X {\n" + declaration + " {\n"
                           + "work();\n" * 79 + "}\n};\n")
                self.assertEqual(len(self.findings("function-lines")), 1)

    def test_declarations_controls_and_global_initializers_are_not_functions(self):
        text = "void declaration();\nstruct Data {\n" + "int field;\n" * 90 + "};\n"
        text += "auto callback = [] {\n" + "work();\n" * 90 + "};\n"
        self.write("core/work.cpp", text)
        self.assertEqual(self.findings("function-lines"), [])

    def test_directive_continuations_do_not_create_or_break_definitions(self):
        self.write("core/work.cpp", '#define BODY \\\n    void fake() { \\\n    fake(); }\n' + function())
        found, = self.findings("function-lines")
        self.assertIn("run", found.subject)
        self.assertEqual(found.value, 81)

    def test_both_preprocessor_branches_are_checked(self):
        self.write("core/work.cpp", "#if FIRST\n" + function("first") + "#else\n"
                   + function("second") + "#endif\n")
        self.assertEqual(len(self.findings("function-lines")), 2)

    def test_physical_module_lines_pair_header_and_source(self):
        self.write("core/work/item.h", "// owner\n" + "int x;\n" * 74)
        self.write("core/work/item.cpp", "int y;\n" * 75)
        self.assertEqual(self.findings("module-comments"), [])
        self.write("core/work/item.cpp", "int y;\n" * 76)
        finding, = self.findings("module-comments")
        self.assertEqual(finding.path, "core/work/item")
        self.assertIn("1/151", finding.message)

    def test_same_stem_in_different_directories_is_not_one_module(self):
        self.write("core/one/item.cpp", "int x;\n" * 100)
        self.write("core/two/item.h", "int y;\n" * 100)
        self.assertEqual(self.findings("module-comments"), [])

    def test_five_percent_comment_boundary_and_trailing_comments(self):
        self.write("core/work.cpp", "int x; // invariant\n" * 10 + "int y;\n" * 190)
        self.assertEqual(self.findings("module-comments"), [])
        self.write("core/work.cpp", "int x; // invariant\n" * 9 + "int y;\n" * 191)
        self.assertEqual(len(self.findings("module-comments")), 1)

    def test_quoted_and_raw_literals_do_not_count_as_comments(self):
        text = 'const char *a = "/* fake */";\n'
        text += 'const char *b = u8R"delim(\n// fake\n/* fake */\n)delim";\n'
        text += 'const char *c = "escaped \\\" // fake";\n'
        self.assertEqual(check.scan_source(text).comment_lines, frozenset())

    def test_comment_lines_are_counted_once_and_keep_source_line_numbers(self):
        text = "/* a */ /* b */ int x;\n/* c\n * d\n */\n// e\n"
        scanned = check.scan_source(text)
        self.assertEqual(scanned.comment_lines, frozenset(range(1, 6)))
        self.assertEqual(scanned.code.count("\n"), text.count("\n"))

    def test_digit_separators_are_not_character_literals(self):
        self.write("core/work.cpp", "constexpr auto limit = 1'000'000 + 0xAB'CD;\n" + function())
        self.assertEqual(len(self.findings("function-lines")), 1)

    def test_prefixed_character_literals_are_not_numeric_separators(self):
        for literal in ("L'a'", "u'a'", "U'a'", "u8'a'"):
            with self.subTest(literal=literal):
                self.write("core/work.cpp", f"auto letter = {literal};\n" + function())
                self.assertEqual(len(self.findings("function-lines")), 1)

    def test_raw_id_shifts_and_masks_are_counted_outside_the_owner(self):
        self.write("core/foundation/kernel_id.h", "return id >> 32U;\n")
        self.write("core/work.cpp", "return (id >> 32UL) + (other >> 32UL);\nreturn id & 0xffffffffULL;\n")
        found = self.findings("id-bits")
        self.assertEqual(len(found), 2)
        self.assertEqual(sum(item.value for item in found), 3)

    def test_id_patterns_in_comments_and_literals_are_ignored(self):
        self.write("core/work.cpp", '// id >> 32U\nconst char *s = "id & 0xFFFFFFFF";\n')
        self.assertEqual(self.findings("id-bits"), [])

    def test_public_header_also_cannot_split_kernel_ids(self):
        self.write("include/ayni/work.hpp", "inline auto epoch(uint64_t id) { return id >> 32U; }\n")
        finding, = self.findings("id-bits")
        self.assertEqual(finding.path, "include/ayni/work.hpp")
        self.assertEqual(self.run_check()[0], 1)

    def test_each_public_code_needs_its_own_adjacent_comment(self):
        self.write("include/ayni/test.h", "/* Meaning. Since 0.20. */\n"
                   "#define AYNI_REASON_FIRST 1\n"
                   "#define AYNI_REASON_SECOND 2 /* Meaning. */\n"
                   "#define AYNI_REASON_THIRD 3\n"
                   "#define AYNI_DENIED ((ayni_status_t)-10)\n")
        found = self.findings("public-code-comment")
        self.assertEqual({item.subject for item in found}, {"AYNI_REASON_THIRD", "AYNI_DENIED"})

    def test_multiline_public_code_accepts_its_trailing_comment(self):
        self.write("include/ayni/test.h", "#define AYNI_STATUS \\\n    ((ayni_status_t)-1) /* refusal */\n")
        self.assertEqual(self.findings("public-code-comment"), [])

    def test_unrelated_macros_and_fake_definitions_are_not_public_codes(self):
        self.write("include/ayni/test.h", '#define AYNI_LIMIT 10\n'
                   '// #define AYNI_REASON_FAKE 2\nconst char *x = "ayni_status_t";\n')
        self.assertEqual(self.findings("public-code-comment"), [])

    def test_baseline_allows_existing_debt_after_lines_move(self):
        self.write("core/work.cpp", function())
        self.record_baseline()
        self.write("core/work.cpp", "// owner\n\n" + function())
        self.assertEqual(self.run_check()[0], 0)

    def test_existing_function_cannot_grow_beyond_baseline(self):
        self.write("core/work.cpp", function())
        self.record_baseline()
        self.write("core/work.cpp", function(body_lines=80))
        result, _, errors = self.run_check()
        self.assertEqual(result, 1)
        self.assertIn("worsened", errors)

    def test_improving_existing_function_is_allowed_without_refresh(self):
        self.write("core/work.cpp", function(body_lines=85))
        self.record_baseline()
        self.write("core/work.cpp", function())
        self.assertEqual(self.run_check()[0], 0)

    def test_resolved_or_removed_debt_must_leave_the_baseline(self):
        path = self.write("core/work.cpp", function())
        self.record_baseline()
        path.unlink()
        result, _, errors = self.run_check()
        self.assertEqual(result, 1)
        self.assertIn("stale baseline", errors)
        self.record_baseline()
        self.assertEqual(self.run_check()[0], 0)

    def test_new_violation_does_not_inherit_another_functions_allowance(self):
        self.write("core/work.cpp", function())
        self.record_baseline()
        self.write("core/work.cpp", function() + function("another"))
        result, _, errors = self.run_check()
        self.assertEqual(result, 1)
        self.assertIn("new function-lines", errors)

    def test_reviewed_non_id_exception_is_exact_and_counted(self):
        self.write("core/codec.cpp", "return value & 0xFFFFFFFFU;\n")
        finding, = self.findings("id-bits")
        baseline = dict(version=1, violations=[], exceptions=[dict(rule=finding.rule,
                        path=finding.path, subject=finding.subject, maximum=1,
                        reason="Batch commit marker low word, not an ID.")])
        self.baseline.write_text(json.dumps(baseline))
        self.assertEqual(self.run_check()[0], 0)
        self.write("core/codec.cpp", "return value & 0xFFFFFFFFU;\n" * 2)
        self.assertEqual(self.run_check()[0], 1)

    def test_invalid_baselines_fail_instead_of_suppressing_findings(self):
        entry = dict(rule="id-bits", path="core/x.cpp", subject="x", maximum=1, reason="reviewed")
        cases = [[], {}, dict(version=True, violations=[], exceptions=[]),
                 dict(version=1, violations=[entry, entry], exceptions=[]),
                 dict(version=1, violations=[dict(entry, reason="")], exceptions=[]),
                 dict(version=1, violations=[dict(entry, maximum=True)], exceptions=[]),
                 dict(version=1, violations=[], exceptions=[dict(entry, rule="function-lines")])]
        for data in cases:
            with self.subTest(data=data):
                self.baseline.write_text(json.dumps(data))
                self.assertEqual(self.run_check()[0], 2)

    def test_missing_root_directories_and_missing_baseline_are_errors(self):
        self.baseline.unlink()
        self.assertEqual(self.run_check()[0], 2)
        (self.root / "core").rmdir()
        self.assertEqual(self.run_check()[0], 2)

    def test_bad_utf8_and_unbalanced_syntax_report_configuration_errors(self):
        path = self.write("core/work.cpp", "void broken() {\n")
        self.assertEqual(self.run_check()[0], 2)
        path.write_bytes(b"\xff")
        self.assertEqual(self.run_check()[0], 2)

    def test_unterminated_literal_and_comment_fail_explicitly(self):
        for text in ('"unfinished', 'R"tag(unfinished', '/* unfinished'):
            with self.subTest(text=text):
                self.write("core/work.cpp", text)
                self.assertEqual(self.run_check()[0], 2)


if __name__ == "__main__":
    unittest.main()
