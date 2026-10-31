# Repository Guidelines

Apply these guidelines to design, implementation, review, refactoring, and delivery. Prioritize correctness and safety, explicit contracts and compatibility, then clarity and maintainability. Meet known resource and timing constraints; justify performance tradeoffs with measurements. Keep changes within the user's task and existing product behavior.

## Project Structure & Module Organization

The implementation baseline includes M1. `core/` implements the Kernel lifecycle, event ingress, work model, journal/replay, storage recovery and maintenance, and the delivered MA-006 authority mechanisms behind the experimental C ABI in `include/ayni/ayni.h`. Current ABI, implementation, validation, and merge status are recorded in `reports/core/m1-progress.md` and its linked ADRs. Native and simulated Hosts provide the storage ports; Full E2 Native/Sim evidence is delivered in #98 (ADR-0076), with E1/E3 evidence in #99/#100 (ADR-0077/0078) and E4 Native/Sim evidence in ADR-0079. The Core Alpha Native report in `reports/core/m1-core-alpha.md` indexes the final configurations, coverage closeout (#102, ADR-0080) and the scoped coverage deviation. Schema 3 closed the storage-compatibility question (ADR-0082). M2 is deferred, and ESP-IDF storage and hardware validation remain outstanding. Contracts are in `contracts/` (`contracts/traceability.md` lists the tests behind each clause), and headers not yet implemented remain drafts in `include/ayni/draft/`. Read `docs/ayni_v1.0.md`, the Ayni AgentOS plan, before contributing; APIs, implementation directories, and platform support remain proposed unless backed by implementation and validation evidence. Preserve its filename when updating the internal document version.

Ayni is a general-purpose native AgentOS. Follow `docs/architecture/foundation-and-experiment-plan.md` and ADR-0083: complete the general foundation before ESP32-S3, Linux system, or other platform experiments. Native/Sim development checks continue throughout. Hosts need no particular device or capability set; voice, vision, motion, the wheeled assistant, and external protocols are optional capability/application experiments. Physical permits, interlocks, and independent stopping still govern deployments that enable the corresponding execution capabilities. Preserve their contracts and evidence without requiring software-only hosts to assemble a hardware stop chain. The old M2–M6 device-product order is a historical mapping, not a prerequisite for every platform; Linux experiments do not depend on ESP32 or ROS. Planning changes do not automatically start implementation, hardware operations, or new test campaigns.

Keep implementations within the planned layout:

- `core/`: portable Kernel state, scheduling, authorization, and recovery, split by invariant owner into `kernel/`, `work/`, `authority/`, `resources/`, `knowledge/`, `catalog/`, `durability/`, and `foundation/`, with entity-named subdirectories such as `work/task/` (ADR-0008; the budget module was cancelled by ADR-0042).
- `include/ayni/`, `contracts/`: public C/C++ interfaces and versioned contracts.
- `strategies/`, `applications/`: variable policies, goal types, scenes, and assistant workflows.
- `hosts/`, `providers/`, `drivers/esp32/`: platform services, asynchronous I/O, and hardware drivers.
- `control/`, `safety/`: permit-checked local execution and the independent stop chain; neither depends on `core/`.
- `runtime/`: the composition root that wires each deployment profile; the only place that selects deployments and assembles concrete Host, Provider, control, and safety components. Hosts, Providers, and adapters may still encapsulate their own platform implementations.
- `targets/`, `ui/web/`: simulator and ESP32 firmware entry points built on `runtime/`, and management UI/assets.
- `tests/`: unit, contract, integration, fault-injection, fuzz, and hardware-in-loop tests.
- `docs/adr/`, `reports/`: architectural decisions and validation evidence.

## Build, Test, and Development Commands

`docs/workflow.md` records the per-package workflow used since M1 (decisions, tests, documents, validation, review, and merge), and `reports/core/m1-progress.md` records the M1 order, PR split, decisions made, and current status; follow both when taking over work. Start from the current "交接状态" section of `reports/core/m1-progress.md`; section 7 of `docs/workflow.md` records the current handoff procedure. `docs/development.md` records the pinned toolchains, CMake presets, and checks. For documentation changes, run `git diff --check`, `python3 tools/check_contracts.py`, and check consistency with the plan.

Before builds or test suites, inspect available host memory, swap pressure, CPU capacity, and other active jobs. Choose explicit build and test parallelism from their combined peak resource budget, including other agents and containers, and leave headroom for the desktop and other applications. Set `CMAKE_BUILD_PARALLEL_LEVEL` and `CTEST_PARALLEL_LEVEL` to the chosen positive job counts (`ctest` runs one test at a time otherwise); when using containers, set CPU and memory limits from that budget. Monitor resource use during execution and reduce concurrency or pause work if pressure rises. Parallel execution is allowed when resources permit; serial execution and a fixed 2 GiB limit are not universal requirements. Do not use unbounded `-j` or choose concurrency from CPU count alone.

Local validation is selected by the changed behavior and dependencies; no local full suite or five-configuration matrix is mandatory before a PR. Ordinary code PRs run the full existing ASan/UBSan suite in CI; additional configurations run only during explicit full validation and weekly runs (see `docs/workflow.md`). Report which local checks actually ran.

Optional Native commands (after setting `CMAKE_BUILD_PARALLEL_LEVEL` and `CTEST_PARALLEL_LEVEL`):

```sh
cmake -S . -B build/native -DCMAKE_BUILD_TYPE=Debug -DAYNI_BUILD_TESTS=ON -DAYNI_HOST=native
cmake --build build/native
ctest --test-dir build/native --output-on-failure
./build/native/targets/simulator/ayni-sim
```

These configure, build, test, and launch the simulator; `ayni-sim` without arguments keeps the lifecycle demo; `--list-normal` and `--case E1-N1 --output NEW_DIRECTORY` run the P6 normal fixtures. The compiled full profile runs HARNESS, E2, E1, E3 and E4 with `--experiment SUITE --output NEW_DIRECTORY`; exact probes require tests enabled. Native/Sim evidence does not complete Core Alpha or hardware acceptance. Firmware builds use `idf.py build` from `targets/esp32_assistant/` after activating the pinned ESP-IDF environment. Document actual toolchain versions and executable checks when adding implementation; do not report planned commands as completed validation.

## Coding Style & Naming Conventions

Target C++17 Core and a C99-compatible public API; disable exceptions/RTTI in Core and the C++ wrapper. Keep STL types, exceptions, OS handles, and third-party SDK types outside the ABI. Use bounded storage and explicit allocation failures. Public interfaces should expose minimal capabilities with explicit inputs, outputs, ownership, lifetimes, limits, cancellation, and failure semantics.

Use types, construction rules, and boundary validation to express identities, units, ranges, and legal states. Keep invariant checks with their owner; avoid implicit state machines built from unrelated boolean flags or scattered duplicate validation.

`.clang-format` defines C/C++ formatting (four-space indentation), and CI checks it with clang-format 18. Use `snake_case` functions/files and the planned `ayni_` public-symbol prefix. Preserve the Chinese prose and heading structure in the implementation plan. CI uses warnings as errors (`AYNI_WARNINGS_AS_ERRORS`); ordinary code PRs run GCC `native-asan` (ASan/UBSan), while Release, Clang, TSan and ESP-IDF run only during periodic or explicitly requested full validation; file paths do not automatically add configurations; clang-tidy runs weekly and on manual dispatch as an advisory job and does not fail the build. Add configurations before claiming further enforcement.

Comments should explain invariants, ownership, design reasons, and non-obvious tradeoffs. Do not introduce unused dependencies or fields, commented-out implementations, or comments that merely narrate the code. Remove obsolete code and comments caused by the current change without expanding into unrelated cleanup.

## Testing Guidelines

Native tests use GoogleTest 1.17.0, fetched and pinned by `cmake/AyniGoogleTest.cmake` (ADR-0012); selected ESP32 experiments will use ESP-IDF's Unity. Add relevant normal-behavior tests for current development. The dated user authorizations, scoped exceptions and their completion boundaries are maintained in `docs/workflow.md` section 2 step 6; consult that single record instead of inferring that a completed exception authorizes later work. By default, mandatory planted-defect/mutation campaigns and new fault-injection or recovery-specific tests remain suspended; do not move them into an automatic package-end requirement. Preserve existing tests and keep them in the CI full suite. Deferred fault/recovery evidence remains explicitly unverified; the plan's hardware/release acceptance claims still require their own evidence. Use descriptive scenario names.

Target Core coverage ≥75% and safety-critical branch coverage ≥80%; record deviations with evidence and an ADR. The user retired the former 90% target globally on 2026-10-30 (ADR-0088); ADR-0080 preserves the historical M1 closeout decision. Distinguish compilation, simulation, hardware, and endurance results. Record versions, configuration, fault points, and traces needed to reproduce results; report unexecuted checks and limitations explicitly. Ask the user about new product, compatibility or acceptance decisions; do not automatically start M2, full MA-038 or hardware operations.

## Commit & Pull Request Guidelines

Use concise, imperative, scoped commit messages, for example `docs: clarify recovery guarantees`. Keep the initial history focused on documentation and repository setup. Open PRs as drafts and mark them ready after relevant local checks and the applicable review (`docs/workflow.md` steps 9–10); CI skips drafts to avoid consuming the shared runners during development. PRs should explain behavior, link relevant issues/plan sections, and report validation and limitations. Include screenshots for UI changes and any recovery traces actually collected; do not create a new recovery test campaign merely for PR evidence while that work is paused. Update contracts, migrations, ADRs, and relevant tests when semantics change; keep affected project documentation consistent.

## Architecture & Security Constraints

Keep Kernel state under one logical owner and I/O asynchronous. Cross-service changes use the commit coordinator; bounded `step` execution must not wait for network, storage, models, or devices. Preserve durable-acceptance and dispatch-barrier guarantees. Never blindly retry `RESULT_UNKNOWN` actions or replay old physical commands during recovery. Make resource ownership, shutdown, cancellation, and cleanup explicit, including partial failure and capacity exhaustion.

Keep credentials and private media out of commits and logs; configure secrets at runtime. Preserve authorization, private-memory boundaries, and the independent local stop path. Mark capabilities implemented only with corresponding validation evidence. Product behavior and hardware constraints remain defined by the implementation plan.

## Failure, Concurrency & Compatibility

- Handle exceptional conditions through explicit error/status results in Core and the C++ wrapper. Preserve classified errors and their causes without leaking sensitive data. Do not swallow failures, fabricate success, hide unfinished capabilities behind placeholders, or silently degrade outside the contract; reject unsupported paths explicitly.
- For asynchronous or concurrent interfaces, define allowed calling threads, reentrancy, callback/completion context, synchronization and ownership, deadlines, cancellation races, event ordering, duplicate/late results, backpressure, and capacity exhaustion. Keep creation, shutdown, and partial-failure cleanup testable.
- Define compatibility and version rules for public interfaces; keep ABI, wire/protocol, and storage versions separate. Preserve observable behavior during refactoring. Breaking changes require affected-consumer analysis, updated contracts and tests, an ADR, and applicable migration and rollback rules.
- Reuse the same portable Core across Hosts. Isolate platform capabilities and lifetimes at narrow boundaries, reject unavailable features explicitly, and validate new Hosts against shared contracts. Extend through supported contracts and adapters; evolve core semantics deliberately when a real requirement demands it.

## Module Design & Dependency Review

- Treat module boundaries as a priority design and review concern before local class, function, or naming cleanup. Identify stable mechanisms, variable policies, external dependencies, and side effects before coding. Split modules by responsibility, capability, ownership of state and invariants, reasons to change, and dependency direction, rather than line counts, file formats, or call sequence.
- Apply Information Hiding: state each module's responsibility, exclusions, public contract, and data/resource owner, and keep representation details private. Place functions and types with the module that understands their semantics and maintains their invariants; do not manipulate another module's internal state.
- Keep domain rules and state transitions in Core; keep UI text, business workflows, storage/network implementations, OS services, drivers, and vendor SDKs at the boundaries. Core must not depend on concrete adapters or upper layers. Use public contracts and avoid dependency cycles, hidden singletons, implicit or uncontrolled global registries, and shared mutable state across modules.
- Review both oversized managers or utility modules and empty forwarding layers or excessive fragmentation. Compare retaining, merging, splitting, or moving responsibilities using evidence and migration cost. Local duplication is acceptable when sharing would couple unrelated reasons to change.
- For significant module or API decisions, use a relevant change scenario, such as replacing a model/storage Provider or adding a Host, to explain which modules change and why. Distinguish design projections from verified behavior; necessary changes across real contracts are not automatically defects.
- Distinguish compile/import, runtime, build, and test dependencies. Once the implementation supports inspection, automate deterministic rules for cycles, Core platform dependencies, and public-interface boundaries in build checks, architecture tests, or CI.

## Abstraction & Function Design

- Look for a suitable existing mechanism before adding shared code; prefer a local improvement over a parallel replacement system.
- Introduce abstractions for real duplication, stable concepts, known variation, or necessary platform/vendor boundaries. Explain what changes they isolate, what coupling they remove, why they are needed now, and their maintenance cost. Otherwise prefer a clear direct implementation.
- Separate mechanisms from policies and prefer composition. Extend through existing contracts or narrow adapters; avoid speculative plugin systems, factory hierarchies, configuration DSLs, and wrappers around every standard-library call. Do not mandate an architecture framework; a single implementation usually needs an interface only at a genuinely unstable external boundary.
- Follow SLAP (Single Level of Abstraction Principle) and Composed Method: functions should express complete, named behaviors or decisions at a consistent semantic level. Keep high-level flow readable and separate unrelated parsing, policy, SDK, retry, and cleanup details; retain tightly related steps together when useful.
- Read call chains top-down and check that names, parameters, return values, ownership, and side effects match each level's responsibility. Review large functions, deep nesting, duplication, meaningless one-line wrappers, excessive delegation, and forwarding chains. Avoid boolean-parameter piles and unnecessary configuration; split only when the new boundary adds semantic value.

## Architecture-Aware Review

- For an explicit code review or architecture audit, default to inspection and findings only. Modify files only when fixes are also requested or already authorized within that review's scope.
- Review top-down: architecture and dependency direction, module boundaries and state ownership, types and public contracts, then functions and call chains. Inspect relevant callers, consumers, and failure paths beyond the changed lines; scale the review to the affected scope and risk.
- Significant findings must include concrete code evidence with file/line references or an execution trace, impact on behavior/contracts/consumers, priority with rationale, a practical improvement with relevant alternatives or tradeoffs, and a validation method. Distinguish demonstrated defects from hypotheses, preferences, and design tradeoffs.
- Prioritize correctness, safety, compatibility, and structural problems over cosmetic issues. Do not invent findings to fill a quota; report when no actionable findings were found and state meaningful verification limits.

## Pre-delivery Design Self-Review

- After a nontrivial code change, proactively perform a design self-review before delivery without waiting for a separate review request. Check module placement and invariant ownership, abstraction levels and call chains, dependency direction, duplicate mechanisms, and unnecessary generalization or layering.
- Correct issues introduced by the change within the authorized scope. Surface important problems requiring broader product, compatibility, or architecture changes with evidence and options rather than expanding the task silently.
- Inspect changed contracts, failure/concurrency/lifecycle paths and portability boundaries, and run relevant checks under the current testing policy; do not treat inspection as execution of deferred fault/recovery tests. Summarize material tradeoffs, remaining issues, and actual validation; do not present a prose self-review as executed tests. Keep the effort proportionate and avoid unrelated repository-wide audits or boilerplate checklists.

## Readability Requirements

These rules apply to `core/` and `include/ayni/`. They are concrete because the
general rules above did not prevent hard-to-read code (#21; #25–#28, see
`reports/core/ma006-a-d-review.md`). A PR that cannot meet one names the
exception and the reason in its How section.

- **Comments.** Every class, struct and public member in a header states what
  it owns or decides. A field states its unit, its "none" value, and the
  invariant that ties it to other fields. A function longer than about 20 lines
  starts with a comment on what it decides and why. A branch that is not
  obvious from the code (a fallback, a status treated as success, a reuse or
  eviction choice, an ordering rule, a check skipped because an earlier one
  holds) says why and cites the contract item or ADR decision. A module (header
  plus source) under `core/` keeps comment lines at or above 5% of its lines
  and close to its neighbours (the median in `core/` is about 12%).
- **Deleted comments.** A deleted comment that is still true is restored where
  the code now is; otherwise the PR says why it no longer holds.
- **One rule, one place.** A validation, computation, encoding or tree walk is
  written once and called. Struct input and byte input share one validator; a
  record or wire layout has one reader and one writer; a call does not
  re-validate what it already validated. Before adding a check, search for an
  existing one (`rg`) and reuse or move it.
- **Named values.** No bare literal other than 0 or 1 used as a count or index.
  Lifecycle and revision values, record and field offsets and sizes, bit shifts
  and masks, limits and thresholds are named constants; record layouts cite
  `contracts/storage/records.md`. The parts of a Kernel ID are read only
  through `core/foundation/kernel_id.h`.
- **Explicit state.** Fields that are set and cleared together become one
  struct or optional value with a comment on its valid combinations, or an
  enum. Do not use 0 or an empty size as an unstated "none" flag. Do not copy a
  value another module owns unless the PR states why and what keeps the copies
  equal. A boolean parameter does not select between two algorithms; write two
  functions. Do not re-run a check and discard its status.
- **Function size.** Split functions over 80 lines into named steps (SLAP,
  above). A dispatcher such as `ayni_kernel::step()` delegates rather than
  grows.
- **Public codes.** Every new status or reason code in `include/ayni/` has a
  one-line meaning and a "Since 0.N" note.
- **Tests.** Test comments name the contract item or ADR decision they verify.

## Review and Status Evidence

- A readability review states a result for each item in
  `docs/workflow.md` section 2 step 10, with file:line evidence, or with what
  was searched when nothing was found. One sentence covering every item is not
  a result. The author's self-check (step 8) reports the same items separately
  in the PR's Review section.
- Planted-defect/mutation campaigns and their evidence tables are suspended
  under the current testing policy. Historical results remain historical evidence.
- Default to one PR per complete feature, with focused commits for prerequisite
  cleanup, records, implementation, API and tests. Split only at independently
  deliverable boundaries or when complexity makes a complete review impractical.
  About 1,500 changed production lines is a review prompt, not an automatic split
  threshold; explain a materially large scope and its review structure in the PR.
- Status documents stay short. The 交接状态 section of
  `reports/core/m1-progress.md` and the 当前状态 paragraph of
  `docs/ayni_v1.0.md` say what is done, what is next and what waits on the
  user. Commands and results, CI run IDs and test
  counts belong in the PR body; the ADR's 验证 section states the outcome and
  links the PR. Coverage is measured once each part has merged, not in every
  PR (`docs/workflow.md` section 2 step 9). Merge-conditional wording
  ("待合并", "随 #N 合并生效", "合并前须全绿") is removed once the PR merges.
- A claim marked verified must be checkable from the repository, the PR or CI.
  Evidence kept outside the repository is labelled as such. Do not add hash
  manifests or container memory peaks to the PR or ADR.
