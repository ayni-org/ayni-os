#!/usr/bin/env python3
"""Collect all GCC 13 JSON TUs; merge only identical ordered branch layouts.

This preserves the P4 scope/method. Raw compiler exception arcs are removed;
defensive, unexecuted and recovery branches are not excluded. Counts describe
instrumented Native code, not hardware acceptance or uninstantiated templates.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import gzip
import json
import shlex
from pathlib import Path
import subprocess
import sys

SOURCE_PATTERNS = ("*.c", "*.cc", "*.cpp", "*.cxx")

SAFETY_PREFIXES = (
    "core/", "control/permit_gate/", "providers/mock/", "providers/mock_execution/",
)

class Coverage:
    """Own the per-source line union and per-function ordered branch-slot union."""

    def __init__(self, root: Path):
        self.root = root.resolve()
        self.lines: dict[tuple, bool] = {}
        self.branches: dict[tuple, list[bool]] = {}
        self.seen: set[str] = set()
        self.gcc_versions: set[str] = set()

    def add(self, document: dict) -> None:
        if document["format_version"] != "1":
            raise ValueError("unsupported gcov JSON format")
        self.gcc_versions.add(document["gcc_version"])
        working = Path(document["current_working_directory"])
        if not working.is_absolute():
            raise ValueError("gcov working directory must be absolute")
        for source in document["files"]:
            path = Path(source["file"])
            if not path.is_absolute():
                path = working / path
            try:
                relative = path.resolve().relative_to(self.root).as_posix()
            except ValueError:
                continue
            self.seen.add(relative)
            for line in source["lines"]:
                number = line["line_number"]
                key = relative, number
                self.lines[key] = self.lines.get(key, False) or line["count"] > 0
                arcs = [arc for arc in line.get("branches", []) if not arc.get("throw", False)]
                if not arcs:
                    continue
                # Function names keep template instantiations separate. Ordered
                # fallthrough flags include length, so different compiler layouts
                # never lend covered slots to one another.
                layout = tuple(arc.get("fallthrough", False) for arc in arcs)
                branch_key = relative, number, line.get("function_name", ""), layout
                slots = self.branches.setdefault(branch_key, [False] * len(arcs))
                for index, arc in enumerate(arcs):
                    slots[index] |= arc["count"] > 0

    def measure(self, prefixes: tuple[str, ...]) -> dict:
        lines = [hit for (path, _), hit in self.lines.items() if path.startswith(prefixes)]
        branches = [hit for (path, _, _, _), slots in self.branches.items()
                    if path.startswith(prefixes) for hit in slots]
        return {"lines": [sum(lines), len(lines)], "branches": [sum(branches), len(branches)]}

    def missing(self, pattern: str, prefixes: tuple[str, ...] = ("core/",)) -> list[str]:
        return sorted({path.relative_to(self.root).as_posix()
                       for prefix in prefixes for path in (self.root / prefix).rglob(pattern)
                       if path.relative_to(self.root).as_posix() not in self.seen})

    def safety_source_gaps(self) -> list[str]:
        return sorted({path for pattern in SOURCE_PATTERNS
                       for path in self.missing(pattern, SAFETY_PREFIXES)})


def run(command: list[str], root: Path) -> str:
    return subprocess.run(command, cwd=root, text=True, capture_output=True, check=True).stdout


def current_notes(build: Path) -> tuple[list[Path], list[str], int]:
    """Use the current compilation database so stale cached objects cannot count."""
    units = json.loads((build / "compile_commands.json").read_text(encoding="utf-8"))
    notes, missing = set(), []
    for unit in units:
        flags = shlex.split(unit["command"])
        if "--coverage" not in flags or "-fprofile-update=atomic" not in flags:
            raise ValueError("compilation database requires coverage with atomic counters")
        allowed = {"-fprofile-update=atomic", "-fprofile-arcs", "-ftest-coverage"}
        for flag in flags:
            if (flag.startswith(("-fprofile", "-fno-profile", "-fno-test-coverage", "-fsanitize"))
                    and flag not in allowed):
                raise ValueError("conflicting profiling or sanitizer compilation argument")
        output = Path(unit["output"])
        # Makefiles may report output relative to the build root, while Ninja
        # uses the command directory. The compiler's -o uses directory on both.
        if "-o" in flags:
            output_index = flags.index("-o") + 1
            if output_index == len(flags):
                raise ValueError("compiler output argument is missing")
            output = Path(flags[output_index])
        if not output.is_absolute():
            output = Path(unit["directory"]) / output
        note = output.with_suffix(".gcno").resolve()
        note.relative_to(build)
        if note.exists():
            notes.add(note)
        else:
            missing.append(unit["file"])
    stale = set(build.rglob("*.gcno")) - notes
    return sorted(notes), sorted(set(missing)), len(stale)


def build_identity(root: Path, build: Path) -> dict:
    """The built metadata target depends on Core; require a clean matching tree."""
    if run(["git", "status", "--porcelain"], root).strip():
        raise ValueError("coverage collection requires a clean source tree")
    revision = run(["git", "rev-parse", "HEAD"], root).strip()
    metadata = subprocess.run(
        [str(build / "tests/measurements/ayni_native_measurements"), "--build-info"],
        cwd=root, text=True, capture_output=True, check=True,
    )
    if metadata.stderr:
        raise ValueError("built metadata emitted diagnostics; check/reset coverage data")
    identity = json.loads(metadata.stdout)
    if identity["source_revision"] != revision or identity["source_dirty"]:
        raise ValueError("built metadata does not match the clean current revision")
    if identity["build_type"] != "Debug" or identity["sanitizer"]:
        raise ValueError("coverage requires Debug without sanitizer")
    return identity


def collect(root: Path, build: Path, raw: Path, gcov: str) -> dict:
    identity = build_identity(root, build)
    notes, units_without_notes, stale_notes = current_notes(build)
    if not notes:
        raise ValueError("no gcno files in the selected build directory")
    raw.mkdir(parents=True, exist_ok=False)
    coverage = Coverage(root)
    with (raw / "collection.log").open("x", encoding="utf-8") as log:
        for index, note in enumerate(notes):
            process = subprocess.run(
                [gcov, "--json-format", "--branch-probabilities", "--stdout", str(note)],
                cwd=root, text=True, capture_output=True,
            )
            log.write(f"{note.relative_to(build)}\n{process.stderr}\n")
            process.check_returncode()
            with gzip.open(raw / f"{index:04d}.json.gz", "wt", encoding="utf-8") as output:
                output.write(process.stdout)
            coverage.add(json.loads(process.stdout))
            if (index + 1) % 50 == 0:
                print(f"gcov {index + 1}/{len(notes)}", flush=True)
    if coverage.gcc_versions != {identity["compiler"]}:
        raise ValueError("gcov data compiler does not match built metadata")
    return {
        "schema": 1,
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "built_identity": identity,
        "gcov": run([gcov, "--version"], root).splitlines()[0],
        "gcc_versions": sorted(coverage.gcc_versions),
        "translation_units": len(notes),
        "compilation_units_without_notes": units_without_notes,
        "stale_notes_ignored": stale_notes,
        "notes_without_data": sum(not note.with_suffix(".gcda").exists() for note in notes),
        "core": coverage.measure(("core/",)),
        "conservative_safety_prefixes": list(SAFETY_PREFIXES),
        "conservative_safety": coverage.measure(SAFETY_PREFIXES),
        "core_cpp_without_gcov": coverage.missing("*.cpp"),
        "conservative_safety_sources_without_gcov": coverage.safety_source_gaps(),
        "core_headers_without_gcov": coverage.missing("*.h"),
        "method": "line union; non-throw arcs; function+ordered-fallthrough layout slot union",
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--raw", type=Path, required=True, help="new raw artifact directory")
    parser.add_argument("--output", type=Path, required=True, help="new summary JSON file")
    parser.add_argument("--gcov", default="gcov-13")
    args = parser.parse_args()
    root = args.root.resolve()
    try:
        if not (root / "core").is_dir():
            raise ValueError("root must contain the Core source tree")
        if args.output.exists():
            raise ValueError("summary output must be a new file")
        result = collect(root, args.build.resolve(), args.raw.resolve(), args.gcov)
        with args.output.open("x", encoding="utf-8") as output:
            json.dump(result, output, ensure_ascii=False, indent=2)
            output.write("\n")
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return 1 if result["core_cpp_without_gcov"] or result["conservative_safety_sources_without_gcov"] else 0
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        print(f"coverage collection failed: {error}", file=sys.stderr)
        return 2

if __name__ == "__main__":
    raise SystemExit(main())
