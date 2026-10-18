# Repository Guidelines

Apply these guidelines to design, implementation, review, refactoring, and delivery. Prioritize correctness and safety, explicit contracts and compatibility, then clarity and maintainability. Meet known resource and timing constraints; justify performance tradeoffs with measurements. Keep changes within the user's task and existing product behavior.

## Project Structure & Module Organization

This checkout is at the documentation and planning stage. Read `docs/ayni_v1.0.md`, the Ayni AgentOS plan, before contributing; APIs, implementation directories, and platform support remain proposed unless backed by implementation and validation evidence. Preserve its filename when updating the internal document version.

Follow the planned layout when implementation begins:

- `core/`: portable Kernel state, scheduling, authorization, and recovery, split by invariant owner into `kernel/`, `work/`, `authority/`, `resources/`, `knowledge/`, `catalog/`, `durability/`, and `foundation/` (ADR-0008).
- `include/ayni/`, `contracts/`: public C/C++ interfaces and versioned contracts.
- `strategies/`, `applications/`: variable policies, goal types, scenes, and assistant workflows.
- `hosts/`, `providers/`, `drivers/esp32/`: platform services, asynchronous I/O, and hardware drivers.
- `control/`, `safety/`: permit-checked local execution and the independent stop chain; neither depends on `core/`.
- `runtime/`: the composition root that wires each deployment profile; the only place that references concrete Host, Provider, control, and safety implementations.
- `targets/`, `ui/web/`: simulator and ESP32 firmware entry points built on `runtime/`, and management UI/assets.
- `tests/`: unit, contract, integration, fault-injection, fuzz, and hardware-in-loop tests.
- `docs/adr/`, `reports/`: architectural decisions and validation evidence.

## Build, Test, and Development Commands

No versioned build scripts or runnable application exist yet. For current documentation changes, run `git diff --check` and check consistency with the plan. Planned native commands:

```sh
cmake -S . -B build/native -DCMAKE_BUILD_TYPE=Debug -DAYNI_BUILD_TESTS=ON -DAYNI_HOST=native
cmake --build build/native -j
ctest --test-dir build/native --output-on-failure
./build/native/targets/simulator/ayni-sim
```

These will configure, build, test, and launch the simulator once implemented. Future firmware builds use `idf.py build` from `targets/esp32_assistant/` after activating the pinned ESP-IDF environment. Document actual toolchain versions and executable checks when adding implementation; do not report planned commands as completed validation.

## Coding Style & Naming Conventions

Target C++17 Core and a C99-compatible public API; disable exceptions/RTTI in Core and the C++ wrapper. Keep STL types, exceptions, OS handles, and third-party SDK types outside the ABI. Use bounded storage and explicit allocation failures. Public interfaces should expose minimal capabilities with explicit inputs, outputs, ownership, lifetimes, limits, cancellation, and failure semantics.

Use types, construction rules, and boundary validation to express identities, units, ranges, and legal states. Keep invariant checks with their owner; avoid implicit state machines built from unrelated boolean flags or scattered duplicate validation.

No formatter configuration exists. For new C/C++ files, use four-space indentation, `snake_case` functions/files, and the planned `ayni_` public-symbol prefix. Preserve the Chinese prose and heading structure in the implementation plan. Planned checks include formatting, compiler warnings, clang-tidy, and ASan/UBSan; add configurations before claiming enforcement.

Comments should explain invariants, ownership, design reasons, and non-obvious tradeoffs. Do not introduce unused dependencies or fields, commented-out implementations, or comments that merely narrate the code. Remove obsolete code and comments caused by the current change without expanding into unrelated cleanup.

## Testing Guidelines

GoogleTest is a candidate, not an installed framework. Verify public behavior, contracts, invariants, boundaries, and failure paths with tests proportionate to the change and risk. Use descriptive scenario names such as `recovery_after_dispatch_barrier`. Plan deterministic fixtures for multi-Agent resource sharing, crash recovery, and model/strategy replacement; follow the plan's E1–E4 gates before full hardware integration.

Target Core coverage ≥75% and safety-critical branch coverage ≥90%; record deviations with evidence and an ADR. Distinguish compilation, simulation, hardware, and endurance results. Record versions, configuration, fault points, and traces needed to reproduce results; report unexecuted checks and limitations explicitly.

## Commit & Pull Request Guidelines

Use concise, imperative, scoped commit messages, for example `docs: clarify recovery guarantees`. Keep the initial history focused on documentation and repository setup. PRs should explain behavior, link relevant issues/plan sections, and report validation and limitations. Include screenshots for UI changes and traces for recovery changes. Update contracts, migrations, ADRs, and relevant tests when semantics change; keep affected project documentation consistent.

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
- Verify changed contracts, failure/concurrency/lifecycle paths, portability boundaries, and the relevant checks. Summarize material tradeoffs, remaining issues, and actual validation; do not present a prose self-review as executed tests. Keep the effort proportionate and avoid unrelated repository-wide audits or boilerplate checklists.
