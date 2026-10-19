#!/usr/bin/env python3
"""Check the contract clauses in contracts/ and keep contracts/traceability.md current (ADR-0013).

A clause is a Markdown table row "| C-<DOMAIN>-<NNN> | rule | counterexample | verification |".
Checks:
  - every clause has four non-empty cells and an ID used by no other clause;
  - every clause lives in the document that contracts/README.md assigns to its prefix;
  - every clause ID mentioned in contracts/, include/, docs/, reports/ or source directories is defined;
  - every GoogleTest name cited as `Suite.test` and every CTest name cited as ctest `name` exists, and
    no defined GoogleTest test is cited by its bare name, which this check could not follow;
  - every step of experiments E1-E4 in the plan is named by at least one clause's verification cell;
  - contracts/traceability.md equals the matrix generated from the clauses.

--write regenerates contracts/traceability.md instead of comparing it.
Directories named in skipped_dir_names of tools/architecture.json (build output, fetched components)
are not scanned. Prints one line per problem. Exit status: 0 clean, 1 problems found, 2 inputs that
cannot be read, including files that are not UTF-8.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

ARCHITECTURE = Path(__file__).resolve().with_name("architecture.json")
PLAN = PurePosixPath("docs/ayni_v1.0.md")
CONTRACTS = PurePosixPath("contracts")
README = CONTRACTS / "README.md"
TRACEABILITY = CONTRACTS / "traceability.md"
# Directories whose text may cite clause IDs. tests/ is excluded: rule tests use synthetic IDs.
REFERENCE_DIRS = ("contracts", "include", "docs", "reports", "core", "hosts", "runtime", "targets", "control", "safety")
REFERENCE_SUFFIXES = {".md", ".h", ".hpp", ".c", ".cc", ".cpp", ".txt", ".cmake"}

CLAUSE_ROW_RE = re.compile(r"^\|\s*(C-[A-Z]+-\d{3})\s*\|")
CLAUSE_ID_RE = re.compile(r"\bC-[A-Z]+-\d{3}\b")
PREFIX_ROW_RE = re.compile(r"^\|\s*((?:`C-[A-Z]+`(?:、)?)+)\s*\|\s*\[[^\]]+\]\(([^)]+)\)\s*\|")
RELATED_RE = re.compile(r"^- \*\*关联\*\*：(.*)$", re.MULTILINE)
SECTION_RE = re.compile(r"§(\d+(?:\.\d+)?)|第 (\d+(?:\.\d+)?) 节")
ADR_RE = re.compile(r"ADR-(\d{4})")
PACKAGE_RE = re.compile(r"MA-(\d{3})")
GTEST_CITE_RE = re.compile(r"`([A-Z][A-Za-z0-9]*)\.([a-z][a-z0-9_]*)`")
BARE_NAME_RE = re.compile(r"`([a-z][a-z0-9_]*)`")
CTEST_CITE_RE = re.compile(r"ctest `([a-z][a-z0-9_]*)`")
GTEST_DEF_RE = re.compile(r"\bTEST(?:_F|_P)?\(\s*(\w+)\s*,\s*(\w+)\s*\)")
CTEST_DEF_RE = re.compile(r"\badd_test\(\s*NAME\s+(\w+)")
DASH = "[–~-]"
STEP_CITE_RE = re.compile(rf"E([134]) 步骤 (\d+(?:{DASH}\d+)?(?:、\d+(?:{DASH}\d+)?)*)")
FAULT_CITE_RE = re.compile(rf"E2 (F\d+(?:{DASH}F?\d+)?(?:、F\d+(?:{DASH}F?\d+)?)*)")
EXPERIMENT_HEADING_RE = re.compile(r"^### [\d.]+ (E[1-4])[：:]")
STEP_ROW_RE = re.compile(r"^\|\s*(\d+)\.\s")
FAULT_ROW_RE = re.compile(r"^\|\s*(F\d+)[：:]")
IMPLEMENTED_MARK = "M0 已实现"


class InputError(Exception):
    pass


@dataclass(frozen=True)
class Clause:
    id: str
    doc: PurePosixPath
    line: int
    rule: str
    counterexample: str
    verification: str

    @property
    def prefix(self) -> str:
        return self.id.rsplit("-", 1)[0]


@dataclass
class Document:
    path: PurePosixPath
    related: str
    clauses: list[Clause]


def read(root: Path, relative: PurePosixPath) -> str:
    return read_path(root / relative, relative)


def read_path(path: Path, shown: PurePosixPath) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except OSError as error:
        raise InputError(f"cannot read {shown}: {error.strerror}") from error
    except UnicodeDecodeError as error:
        raise InputError(f"cannot read {shown}: not UTF-8 ({error.reason} at byte {error.start})") from error


def skipped_dir_names() -> frozenset[str]:
    """Shared with check_deps.py so that both tools skip the same generated trees."""
    shown = PurePosixPath("tools/architecture.json")
    try:
        return frozenset(json.loads(read_path(ARCHITECTURE, shown))["skipped_dir_names"])
    except (json.JSONDecodeError, KeyError, TypeError) as error:
        raise InputError(f"cannot read skipped_dir_names from {shown}: {error}") from error


def walk_files(base: Path, skipped: frozenset[str]):
    for directory, subdirs, files in os.walk(base):
        subdirs[:] = sorted(name for name in subdirs if name not in skipped)
        for name in sorted(files):
            yield Path(directory) / name


def split_row(line: str) -> list[str]:
    return [cell.strip() for cell in line.strip().strip("|").split("|")]


def parse_documents(root: Path, problems: list[str], malformed: set[str]) -> list[Document]:
    """Malformed rows are reported once and their IDs added to malformed, so that
    references to them are not reported again as undefined."""
    documents = []
    for path in sorted((root / CONTRACTS).rglob("*.md")):
        relative = PurePosixPath(path.relative_to(root).as_posix())
        if relative in (README, TRACEABILITY):
            continue
        text = read(root, relative)
        clauses = []
        for number, line in enumerate(text.splitlines(), start=1):
            match = CLAUSE_ROW_RE.match(line)
            if not match:
                continue
            cells = split_row(line)
            if len(cells) != 4:
                problems.append(f"{relative}:{number}: {match.group(1)} has {len(cells)} cells, expected 4 "
                                "(ID, rule, counterexample, verification); a '|' inside a cell splits it")
                malformed.add(match.group(1))
                continue
            if not all(cells):
                problems.append(f"{relative}:{number}: {match.group(1)} has an empty cell")
                malformed.add(match.group(1))
                continue
            clauses.append(Clause(cells[0], relative, number, cells[1], cells[2], cells[3]))
        related = RELATED_RE.search(text)
        if clauses and not related:
            problems.append(f"{relative}: has clauses but no '- **关联**：' line")
        documents.append(Document(relative, related.group(1) if related else "", clauses))
    return documents


def parse_prefixes(root: Path) -> dict[str, PurePosixPath]:
    prefixes = {}
    for line in read(root, README).splitlines():
        match = PREFIX_ROW_RE.match(line)
        if not match:
            continue
        doc = CONTRACTS / match.group(2)
        for prefix in re.findall(r"`(C-[A-Z]+)`", match.group(1)):
            prefixes[prefix] = doc
    if not prefixes:
        raise InputError(f"{README}: no prefix table rows found")
    return prefixes


def experiment_steps(root: Path) -> list[str]:
    """Step labels of E1-E4 in plan order, e.g. "E1 步骤 3" or "E2 F7"."""
    steps = []
    experiment = None
    for line in read(root, PLAN).splitlines():
        heading = EXPERIMENT_HEADING_RE.match(line)
        if heading:
            experiment = heading.group(1)
            continue
        if line.startswith("## ") or line.startswith("### "):
            experiment = None
            continue
        if experiment is None:
            continue
        if experiment == "E2":
            row = FAULT_ROW_RE.match(line)
            if row:
                steps.append(f"E2 {row.group(1)}")
        else:
            row = STEP_ROW_RE.match(line)
            if row:
                steps.append(f"{experiment} 步骤 {row.group(1)}")
    if not steps:
        raise InputError(f"{PLAN}: no experiment step tables found under E1-E4 headings")
    return steps


def expand(items: str, prefix: str = "") -> list[str]:
    numbers = []
    for item in items.split("、"):
        bounds = re.split(DASH, item.replace("F", ""))
        first, last = int(bounds[0]), int(bounds[-1])
        numbers.extend(f"{prefix}{n}" for n in range(first, last + 1))
    return numbers


def cited_steps(verification: str) -> list[str]:
    steps = []
    for match in STEP_CITE_RE.finditer(verification):
        steps.extend(f"E{match.group(1)} 步骤 {n}" for n in expand(match.group(2)))
    for match in FAULT_CITE_RE.finditer(verification):
        steps.extend(f"E2 {n}" for n in expand(match.group(1), "F"))
    return steps


def defined_tests(root: Path, skipped: frozenset[str]) -> tuple[set[tuple[str, str]], set[str]]:
    gtests, ctests = set(), set()
    for path in walk_files(root / "tests", skipped):
        relative = PurePosixPath(path.relative_to(root).as_posix())
        if path.suffix in {".cpp", ".cc"}:
            gtests.update(GTEST_DEF_RE.findall(read_path(path, relative)))
        elif path.name == "CMakeLists.txt":
            ctests.update(CTEST_DEF_RE.findall(read_path(path, relative)))
    return gtests, ctests


def reference_files(root: Path, skipped: frozenset[str]):
    for directory in REFERENCE_DIRS:
        for path in walk_files(root / directory, skipped):
            relative = PurePosixPath(path.relative_to(root).as_posix())
            if path.suffix in REFERENCE_SUFFIXES and relative != TRACEABILITY:
                yield path, relative


def section_key(section: str) -> tuple[int, ...]:
    return tuple(int(part) for part in section.split("."))


def join(items) -> str:
    return "、".join(items) if items else "—"


def generate_traceability(documents: list[Document], steps: list[str]) -> str:
    clauses = [clause for document in documents for clause in document.clauses]
    sections, adrs = defaultdict(set), defaultdict(set)
    for document in documents:
        if not document.clauses:
            continue
        name = document.path.relative_to(CONTRACTS).as_posix()
        for match in SECTION_RE.finditer(document.related):
            sections[match.group(1) or match.group(2)].add(name)
        for adr in ADR_RE.findall(document.related):
            adrs[adr].add(name)
    packages, by_step = defaultdict(list), defaultdict(list)
    for clause in clauses:
        for package in sorted(set(PACKAGE_RE.findall(clause.verification))):
            packages[package].append(clause.id)
        for step in dict.fromkeys(cited_steps(clause.verification)):
            by_step[step].append(clause.id)

    lines = [
        "# 契约追踪矩阵",
        "",
        "本文件由 `tools/check_contracts.py --write` 根据各契约文档生成，请勿手工编辑。修改条款后重新生成；"
        "本文件过期时 CTest 用例 `check_contracts_repository` 失败。",
        "",
        "## 概览",
        "",
        "| 文档 | 条款数 | M0 已实现 |",
        "|---|---:|---:|",
    ]
    for document in documents:
        if document.clauses:
            implemented = sum(IMPLEMENTED_MARK in clause.verification for clause in document.clauses)
            name = document.path.relative_to(CONTRACTS).as_posix()
            lines.append(f"| [{name}]({name}) | {len(document.clauses)} | {implemented} |")
    total_implemented = sum(IMPLEMENTED_MARK in clause.verification for clause in clauses)
    lines += [f"| 合计 | {len(clauses)} | {total_implemented} |", ""]

    lines += ["## 计划章节 → 契约文档", "", "| 计划章节 | 契约文档 |", "|---|---|"]
    for section in sorted(sections, key=section_key):
        lines.append(f"| §{section} | {join(sorted(sections[section]))} |")
    lines += ["", "## ADR → 契约文档", "", "| ADR | 契约文档 |", "|---|---|"]
    for adr in sorted(adrs):
        lines.append(f"| ADR-{adr} | {join(sorted(adrs[adr]))} |")
    lines += ["", "## 工作包 → 条款", "", "| 工作包 | 条款 |", "|---|---|"]
    for package in sorted(packages):
        lines.append(f"| `MA-{package}` | {join(packages[package])} |")
    lines += ["", "## 实验步骤 → 条款", "", "| 实验步骤 | 条款 |", "|---|---|"]
    for step in steps:
        lines.append(f"| {step} | {join(by_step.get(step, []))} |")
    lines += ["", "## M0 已实现的条款", "", "| 条款 | 验证 |", "|---|---|"]
    for clause in clauses:
        if IMPLEMENTED_MARK in clause.verification:
            lines.append(f"| {clause.id} | {clause.verification} |")
    lines.append("")
    return "\n".join(lines)


def check(root: Path, write: bool = False) -> list[str]:
    problems: list[str] = []
    malformed: set[str] = set()
    documents = parse_documents(root, problems, malformed)
    prefixes = parse_prefixes(root)
    steps = experiment_steps(root)
    skipped = skipped_dir_names()
    clauses = [clause for document in documents for clause in document.clauses]

    seen: dict[str, Clause] = {}
    for clause in clauses:
        if clause.id in seen:
            first = seen[clause.id]
            problems.append(f"{clause.doc}:{clause.line}: {clause.id} is already defined at {first.doc}:{first.line}")
            continue
        seen[clause.id] = clause
        expected = prefixes.get(clause.prefix)
        if expected is None:
            problems.append(f"{clause.doc}:{clause.line}: prefix {clause.prefix} is not in the {README} prefix table")
        elif expected != clause.doc:
            problems.append(f"{clause.doc}:{clause.line}: {clause.id} belongs in {expected} per {README}")

    for path, relative in reference_files(root, skipped):
        for number, line in enumerate(read_path(path, relative).splitlines(), start=1):
            for cited in CLAUSE_ID_RE.findall(line):
                if cited not in seen and cited not in malformed:
                    problems.append(f"{relative}:{number}: {cited} is not defined in any contract")

    gtests, ctests = defined_tests(root, skipped)
    suites_by_name = defaultdict(set)
    for suite, name in gtests:
        suites_by_name[name].add(suite)
    for clause in clauses:
        for suite, name in GTEST_CITE_RE.findall(clause.verification):
            if (suite, name) not in gtests:
                problems.append(f"{clause.doc}:{clause.line}: {clause.id} cites test {suite}.{name}, "
                                "which is not defined under tests/")
        for name in BARE_NAME_RE.findall(clause.verification):
            if name in suites_by_name:
                spelled = " or ".join(f"`{suite}.{name}`" for suite in sorted(suites_by_name[name]))
                problems.append(f"{clause.doc}:{clause.line}: {clause.id} cites test `{name}` without its "
                                f"suite; write {spelled} so that a rename is caught")
        for name in CTEST_CITE_RE.findall(clause.verification):
            if name not in ctests:
                problems.append(f"{clause.doc}:{clause.line}: {clause.id} cites ctest {name}, "
                                "which no add_test under tests/ defines")

    covered = {step for clause in clauses for step in cited_steps(clause.verification)}
    for step in steps:
        if step not in covered:
            problems.append(f"{PLAN}: experiment step '{step}' is not named in any clause's verification cell")

    generated = generate_traceability(documents, steps)
    if write:
        (root / TRACEABILITY).write_text(generated, encoding="utf-8")
    else:
        try:
            current = read(root, TRACEABILITY)
        except InputError:
            current = None
        if current != generated:
            problems.append(f"{TRACEABILITY}: out of date; run tools/check_contracts.py --write")
    return problems


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parent.parent, help="repository root")
    parser.add_argument("--write", action="store_true", help="regenerate contracts/traceability.md")
    args = parser.parse_args(argv)
    try:
        problems = check(args.root.resolve(), write=args.write)
    except InputError as error:
        print(f"check_contracts: {error}", file=sys.stderr)
        return 2
    for problem in problems:
        print(problem)
    if problems:
        print(f"check_contracts: {len(problems)} problem(s)", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
