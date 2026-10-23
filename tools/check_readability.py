#!/usr/bin/env python3
"""Check the mechanical readability rules in AGENTS.md against a reviewed baseline.

Only core/ and include/ayni/ are scanned. A module is a header/source group
with the same directory and filename stem; physical lines include blanks.
Function length runs from the first declaration token to its closing brace.
Comments count once per physical line, including trailing comments.

This is a lexical C/C++ check, not a compiler: it reads both preprocessor
branches, skips directives, and recognizes ordinary named definitions,
constructors and inline members. Lambdas are included in their enclosing
function's length. Macro-generated definitions and semantic facts (whether a
comment explains an invariant, or a bit operation really splits a Kernel ID)
still need review. Exact, counted non-ID exceptions require a written reason.

Exit status: 0 no new violations, 1 new/worsened violations or stale baseline,
2 invalid source/configuration. --write-baseline is an explicit review action;
CI never regenerates the baseline. Baseline identity does not use line numbers.
"""

from __future__ import annotations

import argparse
from collections import Counter, defaultdict
from dataclasses import dataclass
import json
from pathlib import Path
import re
import sys

SOURCE_SUFFIXES = {".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp", ".tpp", ".ixx",
                   ".c", ".cc", ".cpp", ".cxx"}
FUNCTION_LINES = 80
MODULE_LINES = 150
COMMENT_PERCENT = 5
ID_HEADER = "core/foundation/kernel_id.h"
TOKEN = re.compile(r"[A-Za-z_]\w*|0[xX][0-9a-fA-F]+[uUlL]*|\d+[uUlL]*|::|->|[^\s]")
CONTROL = {"if", "for", "while", "switch", "catch", "sizeof", "alignof", "alignas",
           "decltype", "noexcept", "static_assert"}
ID_BITS = re.compile(r">>\s*32(?:[uUlL]+)?\b|&\s*0[xX][fF]{8}(?:[uUlL]+)?\b")
DEFINE = re.compile(r"^[ \t]*#[ \t]*define[ \t]+(AYNI_\w+)\b((?:\\\r?\n|[^\n])*)",
                    re.MULTILINE)


class CheckError(Exception):
    pass


@dataclass(frozen=True)
class Source:
    code: str  # Comments/literals replaced by spaces, preserving offsets/newlines.
    comment_lines: frozenset[int]
    lines: int


def scan_source(text: str) -> Source:
    """Keep source positions while hiding comments and every C++ string form."""
    chars = list(text)
    comments = set()
    index, line = 0, 1
    while index < len(text):
        start = index
        comment = False
        if text.startswith("//", index):
            end = text.find("\n", index)
            end = len(text) if end < 0 else end
            while end < len(text) and text[end - 1:end] == "\\":
                next_end = text.find("\n", end + 1)
                end = len(text) if next_end < 0 else next_end
            comment = True
        elif text.startswith("/*", index):
            end = text.find("*/", index + 2)
            if end < 0:
                raise CheckError("unterminated block comment")
            end += 2
            comment = True
        elif text.startswith('R"', index):
            opening = text.find("(", index + 2, index + 19)
            if opening < 0:
                raise CheckError("invalid raw string delimiter")
            marker = ")" + text[index + 2:opening] + '"'
            end = text.find(marker, opening + 1)
            if end < 0:
                raise CheckError("unterminated raw string")
            end += len(marker)
        elif text[index] in "\"'":
            # Apostrophes inside numeric literals are digit separators.
            if text[index] == "'":
                number_start = index
                while number_start and (text[number_start - 1].isalnum()
                                        or text[number_start - 1] in "._'"):
                    number_start -= 1
                if (number_start < index and text[number_start].isdigit()
                        and index + 1 < len(text) and text[index + 1].isalnum()):
                    index += 1
                    continue
            quote = text[index]
            end = index + 1
            while end < len(text) and text[end] != quote:
                end += 2 if text[end] == "\\" else 1
            if end >= len(text):
                raise CheckError("unterminated quoted literal")
            end += 1
        else:
            line += text[index] == "\n"
            index += 1
            continue
        for offset in range(start, end):
            if comment and text[offset] != "\n":
                comments.add(line)
            if text[offset] == "\n":
                line += 1
            else:
                chars[offset] = " "
        index = end
    return Source("".join(chars), frozenset(comments), len(text.splitlines()))


@dataclass(frozen=True)
class Function:
    subject: str
    first: int
    last: int


def strip_directives(code: str) -> str:
    lines = []
    continuation = False
    for line in code.splitlines(keepends=True):
        directive = continuation or line.lstrip().startswith("#")
        continuation = directive and line.rstrip("\r\n").endswith("\\")
        lines.append(re.sub(r"[^\n]", " ", line) if directive else line)
    return "".join(lines)


def parameter_end(words: list[str]) -> int | None:
    """Find a named declarator, excluding controls, lambdas and initializers."""
    depth = 0
    brackets = 0
    angles = 0
    template_name = ""
    initializer = False
    candidate = False
    for index, word in enumerate(words):
        if word == "[":
            brackets += 1
        elif word == "]":
            brackets -= 1
        if brackets:
            continue
        previous = words[index - 1] if index else ""
        if depth == 0:
            if word == "<" and "operator" not in words[:index]:
                if angles == 0:
                    template_name = previous
                angles += 1
            elif word == ">" and angles:
                angles -= 1
            elif word == "=" and not angles:
                initializer = True
        if word == "(" and depth == 0:
            name = template_name if previous == ">" else previous
            candidate = (bool(re.fullmatch(r"[A-Za-z_]\w*", name))
                         and name not in CONTROL and not initializer and not angles)
            # operator() has a name before its first parameter-like pair.
            if previous == "operator" and words[index:index + 3] == ["(", ")", "("]:
                candidate = False
            if previous == ")" and "operator" in words[:index]:
                candidate = True
            if "operator" in words[:index] and previous not in CONTROL:
                candidate = words[index:index + 3] != ["(", ")", "("]
        if word == "(":
            depth += 1
        elif word == ")":
            depth -= 1
            if depth == 0 and candidate:
                return index
    if angles and not initializer:
        raise CheckError("unbalanced template delimiters; parenthesize comparison/shift defaults")
    return None


def functions(source: Source) -> list[Function]:
    code = strip_directives(source.code)
    tokens = list(TOKEN.finditer(code))
    words = [token.group() for token in tokens]
    brace_pairs = {}
    stack = []
    for index, word in enumerate(words):
        if word == "{":
            stack.append(index)
        elif word == "}":
            if not stack:
                raise CheckError("unmatched closing brace (conditional syntax needs review)")
            brace_pairs[stack.pop()] = index
    if stack:
        raise CheckError("unmatched opening brace (conditional syntax needs review)")
    result = []

    def visit(begin: int, end: int, context: tuple[str, ...]) -> None:
        start, index = begin, begin
        while index < end:
            word = words[index]
            if word == ";":
                start = index + 1
            elif word == ":" and words[start:index] in (["public"], ["private"], ["protected"]):
                start = index + 1
            elif word == "{":
                close = brace_pairs[index]
                header = words[start:index]
                parameters = parameter_end(header)
                if header[:1] == ["namespace"] or header[:2] == ["inline", "namespace"]:
                    visit(index + 1, close, context + (" ".join(header),))
                elif parameters is not None:
                    tail = header[parameters + 1:]
                    # A constructor's braced member initializer precedes its body.
                    if ":" in tail and tail[-1:] not in ([")"], ["}"]):
                        index = close + 1
                        continue
                    signature = header[:parameters + 1]
                    if ":" not in tail:
                        signature += tail
                    subject = " / ".join((*context, " ".join(signature)))
                    result.append(Function(subject, code.count("\n", 0, tokens[start].start()) + 1,
                                           code.count("\n", 0, tokens[close].end()) + 1))
                elif any(item in header for item in ("class", "struct", "union")):
                    visit(index + 1, close, context + (" ".join(header),))
                elif header[:1] == ["extern"]:
                    visit(index + 1, close, context)
                index, start = close, close + 1
            index += 1

    visit(0, len(words), ())
    return result


@dataclass(frozen=True)
class Finding:
    rule: str
    path: str
    subject: str
    line: int
    value: int
    message: str

    @property
    def key(self) -> tuple[str, str, str]:
        return self.rule, self.path, self.subject


def inspect(root: Path) -> list[Finding]:
    findings = []
    modules = defaultdict(lambda: [0, 0])
    for directory in ("core", "include/ayni"):
        if not (root / directory).is_dir():
            raise CheckError(f"missing source directory: {directory}")
        for path in sorted((root / directory).rglob("*")):
            if not path.is_file() or path.suffix not in SOURCE_SUFFIXES:
                continue
            relative = path.relative_to(root).as_posix()
            try:
                source = scan_source(path.read_text(encoding="utf-8"))
                if directory == "core":
                    for function in functions(source):
                        length = function.last - function.first + 1
                        if length > FUNCTION_LINES:
                            findings.append(Finding("function-lines", relative, function.subject,
                                                    function.first, length,
                                                    f"function has {length} lines (limit {FUNCTION_LINES})"))
                    module = path.relative_to(root).with_suffix("").as_posix()
                    modules[module][0] += source.lines
                    modules[module][1] += len(source.comment_lines)
                else:
                    for match in DEFINE.finditer(source.code):
                        name, body = match.groups()
                        if not name.startswith("AYNI_REASON_") and "ayni_status_t" not in body:
                            continue
                        first = source.code.count("\n", 0, match.start()) + 1
                        last = source.code.count("\n", 0, match.end()) + 1
                        own_comment = any(line in source.comment_lines for line in range(first, last + 1))
                        previous_comment = (first - 1 in source.comment_lines and first > 1
                                            and not source.code.splitlines()[first - 2].strip())
                        if not own_comment and not previous_comment:
                            findings.append(Finding("public-code-comment", relative, name, first, 1,
                                                    f"{name} has no adjacent comment"))
                if relative != ID_HEADER:
                    bits = defaultdict(list)
                    for match in ID_BITS.finditer(source.code):
                        line = source.code.count("\n", 0, match.start()) + 1
                        statement = source.code.splitlines()[line - 1]
                        subject = " ".join(TOKEN.findall(statement))
                        bits[subject].append(line)
                    for subject, lines in bits.items():
                        findings.append(Finding("id-bits", relative, subject, lines[0], len(lines),
                                                f"{len(lines)} raw ID-bit pattern(s) outside {ID_HEADER}"))
            except CheckError as error:
                raise CheckError(f"{relative}: {error}") from None
    for module, (lines, comments) in sorted(modules.items()):
        if lines > MODULE_LINES and comments * 100 < lines * COMMENT_PERCENT:
            findings.append(Finding("module-comments", module, "header + source", 1,
                                    10000 - comments * 10000 // lines,
                                    f"{comments}/{lines} comment lines ({comments * 100 / lines:.2f}%, minimum 5%)"))
    keys = [item.key for item in findings]
    if len(keys) != len(set(keys)):
        raise CheckError("duplicate finding identity; refine the source declaration or checker")
    return sorted(findings, key=lambda item: item.key)


def load_baseline(path: Path) -> dict:
    data = json.loads(path.read_text(encoding="utf-8"))
    if (not isinstance(data, dict) or type(data.get("version")) is not int
            or data["version"] != 1):
        raise CheckError("baseline must be an object with version 1")
    seen = set()
    for group in ("violations", "exceptions"):
        entries = data.get(group)
        if not isinstance(entries, list):
            raise CheckError(f"baseline {group} must be an array")
        for entry in entries:
            if not isinstance(entry, dict):
                raise CheckError("baseline entry must be an object")
            for field in ("rule", "path", "subject", "reason"):
                if not isinstance(entry.get(field), str) or not entry[field].strip():
                    raise CheckError(f"baseline entry needs nonempty {field}")
            if type(entry.get("maximum")) is not int or entry["maximum"] <= 0:
                raise CheckError("baseline maximum must be a positive integer")
            if group == "exceptions" and entry["rule"] != "id-bits":
                raise CheckError("only reviewed non-ID bit patterns may be exceptions")
            key = tuple(entry[field] for field in ("rule", "path", "subject"))
            if key in seen:
                raise CheckError(f"duplicate baseline entry: {key}")
            seen.add(key)
    return data


def compare(findings: list[Finding], baseline: dict) -> list[str]:
    expected = {tuple(entry[field] for field in ("rule", "path", "subject")): entry
                for group in ("violations", "exceptions") for entry in baseline[group]}
    errors = []
    for item in findings:
        entry = expected.pop(item.key, None)
        if entry is None or item.value > entry["maximum"]:
            kind = "new" if entry is None else "worsened"
            errors.append(f"{item.path}:{item.line}: {kind} {item.rule}: {item.message}; {item.subject}")
    for key in sorted(expected):
        errors.append(f"{key[1]}:1: stale baseline {key[0]}: remove {key[2]}")
    return errors


def write_baseline(path: Path, findings: list[Finding], baseline: dict) -> None:
    exceptions = {tuple(entry[field] for field in ("rule", "path", "subject")): entry
                  for entry in baseline["exceptions"]}
    entries = []
    for item in findings:
        if item.key in exceptions:
            continue
        entries.append(dict(rule=item.rule, path=item.path, subject=item.subject,
                            maximum=item.value,
                            reason="Existing debt; resolve and remove with the owning change."))
    data = dict(baseline, violations=entries)
    path.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--baseline", type=Path, help="default: ROOT/tools/readability_baseline.json")
    parser.add_argument("--write-baseline", action="store_true", help="record current debt for review")
    parser.add_argument("--list", action="store_true", help="show every finding, including existing debt")
    args = parser.parse_args(argv)
    path = args.baseline or args.root / "tools/readability_baseline.json"
    try:
        findings = inspect(args.root)
        baseline = load_baseline(path)
        if args.write_baseline:
            write_baseline(path, findings, baseline)
            baseline = load_baseline(path)
        if args.list:
            for item in findings:
                print(f"{item.path}:{item.line}: {item.rule}: {item.message}; {item.subject}")
        errors = compare(findings, baseline)
        for error in errors:
            print(error, file=sys.stderr)
        if errors:
            return 1
        exceptions = {tuple(entry[field] for field in ("rule", "path", "subject"))
                      for entry in baseline["exceptions"]}
        counts = Counter(item.rule for item in findings if item.key not in exceptions)
        print(f"Mechanical readability checks: no new violations; "
              f"{sum(counts.values())} existing finding(s) {dict(sorted(counts.items()))}; "
              f"{len(exceptions)} reviewed non-ID exception(s). Human review is still required.")
        return 0
    except (CheckError, OSError, UnicodeError, json.JSONDecodeError) as error:
        print(f"readability: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
