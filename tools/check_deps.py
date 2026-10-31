#!/usr/bin/env python3
"""Check #include dependencies against tools/architecture.json (ADR-0011).

Rules:
  core/      Core modules include only strictly lower layers (ADR-0008), their
             own entity subdirectories as declared in core_subdir_deps, the
             implemented public API and core_std_headers. No draft, Host,
             runtime or platform headers. Within a module the files at its
             root count as one more subdirectory, MODULE_ROOT: every include
             between two of its directories, root included, must be declared,
             and the declared graph of each module must have no cycle
             (ADR-0051).
  include/   Implemented public headers include only public_c_headers and other
             implemented public headers; drafts may also include drafts.
  others     Each listed top-level directory includes only the header kinds in
             "components". Core-internal headers are never allowed outside core/.
  tests/     Not checked: tests may reach any layer.

Every C/C++ suffix in SOURCE_SUFFIXES is read. In core/ and include/ any other
file type is a violation unless it is documentation or build script
(NON_SOURCE_SUFFIXES), so that a header with an unexpected suffix cannot escape
the check. A source file that is not UTF-8 is a violation, not a crash.

Prints one line per violation as "path:line: message". Exit status: 0 clean,
1 violations found, 2 invalid configuration.
"""

from __future__ import annotations

import argparse
import json
import os
import posixpath
import re
import sys
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

INCLUDE_RE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*([<"])([^>"\n]+)[>"]', re.MULTILINE)
SOURCE_SUFFIXES = {".h", ".hh", ".hpp", ".hxx", ".inl", ".ipp", ".tpp", ".ixx", ".c", ".cc", ".cpp", ".cxx"}
STRICT_DIRS = {"core", "include"}
NON_SOURCE_SUFFIXES = {".md", ".txt", ".cmake"}
FIXED_RULE_DIRS = {"core", "include", "tests"}
# Names the files directly in a Core module's directory in core_subdir_deps.
MODULE_ROOT = "_root"


class ConfigError(Exception):
    pass


@dataclass(frozen=True)
class Include:
    line: int
    quoted: bool
    path: str


@dataclass(frozen=True)
class Target:
    """What an include refers to. kind is one of: core, api, draft, hosts,
    runtime, external, local, foreign."""

    kind: str
    module: str | None = None  # core module, or instance for hosts/runtime/providers
    subdir: str | None = None  # core entity subdirectory
    owner: str | None = None  # top-level directory for kind == "foreign"


class Architecture:
    def __init__(self, data: dict):
        try:
            self.layer_of: dict[str, int] = {}
            for index, layer in enumerate(data["core_layers"]):
                for module in layer:
                    if module in self.layer_of:
                        raise ConfigError(f"core module '{module}' listed twice")
                    self.layer_of[module] = index
            self.subdir_deps: dict[str, dict[str, list[str]]] = data["core_subdir_deps"]
            self.core_std_headers = set(data["core_std_headers"])
            self.public_c_headers = set(data["public_c_headers"])
            self.public_cpp_headers = set(data.get("public_cpp_headers", []))
            self.header_kinds: dict[str, str] = data["header_kinds"]
            self.components: dict[str, list[str]] = data["components"]
            self.unchecked_dirs = set(data["unchecked_dirs"])
            self.skipped_dir_names = set(data["skipped_dir_names"])
        except KeyError as missing:
            raise ConfigError(f"missing key {missing}") from None
        for module, graph in self.subdir_deps.items():
            if module not in self.layer_of:
                raise ConfigError(f"core_subdir_deps names unknown module '{module}'")
            cycle = find_cycle(graph)
            if cycle:
                raise ConfigError(f"core_subdir_deps of '{module}' has a cycle: {' -> '.join(cycle)}")
        overlap = FIXED_RULE_DIRS & set(self.components)
        if overlap:
            raise ConfigError(f"{sorted(overlap)} have fixed rules and cannot be listed in components")

    @classmethod
    def load(cls, path: Path) -> "Architecture":
        try:
            return cls(json.loads(path.read_text(encoding="utf-8")))
        except (OSError, json.JSONDecodeError) as error:
            raise ConfigError(f"{path}: {error}") from None

    def header_kind(self, include_path: str) -> str | None:
        for prefix in sorted(self.header_kinds, key=len, reverse=True):
            if include_path.startswith(prefix):
                return self.header_kinds[prefix]
        return None


def find_cycle(graph: dict[str, list[str]]) -> list[str]:
    """A cycle of the directed graph, closed by repeating its first node, or [] if none."""
    done: set[str] = set()
    path: list[str] = []

    def visit(node: str) -> list[str]:
        if node in path:
            return path[path.index(node):] + [node]
        if node in done:
            return []
        path.append(node)
        for target in graph.get(node, []):
            cycle = visit(target)
            if cycle:
                return cycle
        path.pop()
        done.add(node)
        return []

    for node in sorted(graph):
        cycle = visit(node)
        if cycle:
            return cycle
    return []


def parse_includes(text: str) -> list[Include]:
    includes = []
    for match in INCLUDE_RE.finditer(text):
        line = text.count("\n", 0, match.start()) + 1
        includes.append(Include(line, match.group(1) == '"', match.group(2).strip()))
    return includes


def instance_of(parts: tuple[str, ...]) -> str | None:
    return parts[1] if len(parts) > 2 else None


def classify(arch: Architecture, root: Path, source: PurePosixPath, include: Include) -> Target:
    if include.quoted:
        resolved = (root / source.parent / include.path).resolve()
        if resolved.is_file() and resolved.is_relative_to(root.resolve()):
            return classify_resolved(arch, source, PurePosixPath(resolved.relative_to(root.resolve()).as_posix()))

    # Include-root spellings name the same boundary after collapsing dot segments.
    # Otherwise service could hide a runtime dependency behind ports/../runtime.
    include_path = posixpath.normpath(include.path)
    parts = PurePosixPath(include_path).parts
    if len(parts) >= 2 and parts[0] in arch.layer_of:
        resolved = (root / "core" / include.path).resolve()
        if resolved.is_file() and resolved.is_relative_to(root.resolve()):
            return classify_resolved(arch, source, PurePosixPath(resolved.relative_to(root.resolve()).as_posix()))
        return Target("core", parts[0], core_directory(arch, parts[0], parts[1:-1]))
    kind = arch.header_kind(include_path)
    if kind is None:
        return Target("external")
    if kind in ("hosts", "runtime", "providers", "strategies", "applications"):
        name = parts[2] if len(parts) > 2 else None
        if name is not None and PurePosixPath(name).suffix in SOURCE_SUFFIXES:
            name = PurePosixPath(name).stem
        return Target(kind, name)
    return Target(kind)


def classify_resolved(arch: Architecture, source: PurePosixPath, target: PurePosixPath) -> Target:
    """A quoted include that names a file in this repository by relative path."""
    source_parts, target_parts = source.parts, target.parts
    if target_parts[0] == "include":
        public_path = PurePosixPath(*target_parts[1:]).as_posix()
        return Target(arch.header_kind(public_path) or "foreign", owner="include")
    if target_parts[0] == "core":
        if len(target_parts) >= 3 and target_parts[1] in arch.layer_of:
            subdir = core_directory(arch, target_parts[1], target_parts[2:-1])
            return Target("core", target_parts[1], subdir)
        return Target("foreign", owner="core")
    same_instance = source_parts[0] == target_parts[0] and (
        source_parts[0] not in ("hosts", "runtime", "providers", "strategies", "applications") or instance_of(source_parts) == instance_of(target_parts)
    )
    if same_instance:
        return Target("local")
    return Target("foreign", owner="/".join(target_parts[:2]))


def module_dir(module: str, subdir: str) -> str:
    if subdir == MODULE_ROOT:
        return f"the root of core/{module} ('{MODULE_ROOT}')"
    return f"core/{module}/{subdir}"


def core_directory(arch: Architecture, module: str, parts: tuple[str, ...]) -> str:
    """Use the most specific declared boundary, including nested kernel partitions."""
    if not parts:
        return MODULE_ROOT
    graph = arch.subdir_deps.get(module, {})
    boundaries = set(graph).union(target for targets in graph.values() for target in targets)
    for count in range(len(parts), 0, -1):
        candidate = "/".join(parts[:count])
        if candidate in boundaries:
            return candidate
    return parts[0]


def check_core(arch: Architecture, source: PurePosixPath, include: Include, target: Target) -> str | None:
    module = source.parts[1]
    subdir = core_directory(arch, module, source.parts[2:-1])
    if target.kind == "core":
        if target.module == module:
            target_subdir = target.subdir or MODULE_ROOT
            if target_subdir == subdir:
                return None
            allowed = arch.subdir_deps.get(module, {}).get(subdir, [])
            if target_subdir in allowed:
                return None
            return (f"{module_dir(module, subdir)} may not include {module_dir(module, target_subdir)}: "
                    f"not declared in core_subdir_deps")
        if arch.layer_of[target.module] > arch.layer_of[module]:
            return None
        return (f"core/{module} may not include core/{target.module}: dependencies point to "
                f"strictly lower layers only (ADR-0008)")
    if target.kind == "api":
        return None
    if target.kind == "external" and not include.quoted and include.path in arch.core_std_headers:
        return None
    reason = ": not in core_std_headers" if target.kind == "external" else ""
    return f"Core may not include {describe(include, target)}{reason}"


def check_public_header(arch: Architecture, source: PurePosixPath, include: Include, target: Target) -> str | None:
    is_draft = source.as_posix().startswith("include/ayni/draft/")
    if target.kind == "api" or (is_draft and target.kind == "draft"):
        return None
    if target.kind == "external" and not include.quoted and include.path in arch.public_c_headers:
        return None
    if source.as_posix() in {"include/ayni/ayni.hpp", "include/ayni/values.hpp", "include/ayni/values_json.hpp", "include/ayni/model_messages.hpp", "include/ayni/operation_json.hpp", "include/ayni/detail/tool_usage.hpp", "include/ayni/workspace.hpp", "include/ayni/detail/workspace.hpp", "include/ayni/call_result_json.hpp"} and target.kind == "external" and not include.quoted and include.path in arch.public_cpp_headers:
        return None
    which = "draft" if is_draft else "implemented public"
    return f"{which} header may not include {describe(include, target)}"


def check_component(arch: Architecture, source: PurePosixPath, include: Include, target: Target) -> str | None:
    component = source.parts[0]
    allowed = arch.components[component]
    if target.kind == "local":
        return None
    if target.kind in allowed:
        return None
    if "own" in allowed and target.kind == component and target.module == instance_of(source.parts):
        return None
    if f"{target.kind}:{target.module}" in allowed:
        return None
    return f"{component}/ may not include {describe(include, target)}"


def describe(include: Include, target: Target) -> str:
    spelled = f'"{include.path}"' if include.quoted else f"<{include.path}>"
    if target.kind == "core":
        return f"{spelled} (Core-internal header of core/{target.module})"
    if target.kind == "foreign":
        return f"{spelled} (file of {target.owner}/)"
    if target.kind == "external":
        return spelled
    return f"{spelled} ({target.kind} header)"


def iter_files(arch: Architecture, root: Path):
    for directory, subdirs, files in os.walk(root):
        relative_dir = PurePosixPath(Path(directory).relative_to(root).as_posix())
        subdirs[:] = sorted(
            name for name in subdirs
            if name not in arch.skipped_dir_names
            and not (relative_dir == PurePosixPath(".") and name in arch.unchecked_dirs)
        )
        for name in sorted(files):
            yield Path(directory) / name, relative_dir / name


def check(root: Path, arch: Architecture) -> list[str]:
    violations = []
    for path, relative in iter_files(arch, root):
        top = relative.parts[0]
        if relative.suffix not in SOURCE_SUFFIXES:
            if top in STRICT_DIRS and len(relative.parts) > 1 and relative.suffix not in NON_SOURCE_SUFFIXES:
                violations.append(f"{relative}: file type '{relative.suffix or 'none'}' is not read by the include check; "
                                  f"use a suffix from SOURCE_SUFFIXES in tools/check_deps.py")
            continue
        if len(relative.parts) == 1:
            violations.append(f"{relative}: source files at the repository root have no dependency rule")
            continue
        if top == "core":
            if len(relative.parts) < 3 or relative.parts[1] not in arch.layer_of:
                violations.append(f"{relative}: Core sources must live in one of the modules in core_layers")
                continue
            rule = check_core
        elif top == "include":
            rule = check_public_header
        elif top in arch.components:
            rule = check_component
        else:
            violations.append(f"{relative}: no include rule for {top}/; add it to tools/architecture.json")
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError as error:
            violations.append(f"{relative}: not UTF-8 ({error.reason} at byte {error.start}); includes not checked")
            continue
        for include in parse_includes(text):
            message = rule(arch, relative, include, classify(arch, root, relative, include))
            if message is not None:
                violations.append(f"{relative}:{include.line}: {message}")
    return violations


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    default_root = Path(__file__).resolve().parent.parent
    parser.add_argument("--root", type=Path, default=default_root, help="repository root")
    parser.add_argument("--architecture", type=Path, help="defaults to <root>/tools/architecture.json")
    args = parser.parse_args(argv)
    root = args.root.resolve()
    try:
        arch = Architecture.load(args.architecture or root / "tools" / "architecture.json")
    except ConfigError as error:
        print(f"check_deps: invalid configuration: {error}", file=sys.stderr)
        return 2
    violations = check(root, arch)
    for violation in violations:
        print(violation)
    if violations:
        print(f"check_deps: {len(violations)} violation(s)", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
