<p align="center">
  <img src="docs/assets/ayni-logo.png" alt="Ayni AgentOS" width="420">
</p>

<h3 align="center">General-Purpose Native AgentOS in C++17 &amp; C99</h3>

<p align="center">
  <strong>Shared systems. Capabilities on demand.</strong><br>
  A portable multi-agent OS kernel with local authorization, bounded memory, typed capabilities, and crash-safe recovery — from Linux hosts to ESP32-S3 microcontrollers.
</p>

<p align="center">
  <a href="https://github.com/ayni-org/ayni-os/actions/workflows/ci.yml"><img alt="CI" src="https://github.com/ayni-org/ayni-os/actions/workflows/ci.yml/badge.svg"></a>
  <a href="LICENSE"><img alt="License: Apache-2.0" src="https://img.shields.io/badge/License-Apache_2.0-blue.svg"></a>
  <a href="docs/development.md"><img alt="Core: C++17" src="https://img.shields.io/badge/Core-C%2B%2B17-00599C?logo=cplusplus&logoColor=white"></a>
  <a href="include/ayni/ayni.h"><img alt="API: C99 ABI 0.62" src="https://img.shields.io/badge/API-C99%20ABI%200.62-555555?logo=c&logoColor=white"></a>
  <a href="contracts/storage/records.md"><img alt="Storage: Schema 12" src="https://img.shields.io/badge/Storage-Schema%2012-2ea44f"></a>
</p>

<p align="center">
  <strong>English</strong> ·
  <a href="README.zh-CN.md">简体中文</a>
</p>

<p align="center">
  <a href="#why-ayni">Why Ayni</a> ·
  <a href="#key-capabilities">Capabilities</a> ·
  <a href="#quick-start">Quick Start</a> ·
  <a href="#architecture">Architecture</a> ·
  <a href="#implementation-and-roadmap">Roadmap</a> ·
  <a href="#documentation">Docs</a>
</p>

---

**Ayni** is a **general-purpose native AgentOS** built for running multiple AI agents safely and deterministically on a shared host. Running above the host operating system, Ayni provides a portable, zero-exception, zero-RTTI **C++17 Core** behind a **C99 ABI** and **C++17 SDK** that owns agent lifecycle, goals, tasks, authorization grants, resource admission, private memory, and durable crash recovery.

Instead of letting prompts or cloud LLM services own tasks, memory, or hardware execution, **models only propose actions — the local Ayni Kernel governs, admits, commits, and recovers.** Capabilities (software jobs, model APIs, event streams, or optional physical devices) plug in through versioned, typed public contracts. The same portable Core powers both software-only Native Linux deployments and constrained embedded experiments such as ESP32-S3.

> [!NOTE]
> **Current Baseline (ABI 0.62 / Core Storage Schema 12):** The general capability foundation and standalone source SDK are delivered with Native/Sim software validation in the [foundation acceptance report](reports/core/foundation-acceptance.md) and [independent integration report](reports/core/independent-consumption.md). Recent work adds an independent unresolved-operation ledger with `QUERY`/`ACK` reconciliation and post-durability audit ([ADR-0115](docs/adr/0115-unknown-operation-history-and-work-retirement.md)), plus decoupled async UI, streaming control sessions, and parameterized reflex rules ([ADR-0114](docs/adr/0114-async-ui-streaming-control-and-parameterized-reflex.md)). The ESP32-S3 touchscreen Wi-Fi/voice setup runs as an independent experiment; full hardware stop-chain and endurance acceptance remain incomplete.

## Why Ayni?

Most AI agent frameworks are high-level orchestration scripts where permissions live in system prompts, state lives in ad-hoc memory, and crashes cause blind retries or lost side effects. Ayni treats multi-agent execution as a **systems engineering problem**:

| Dimension | Typical Agent Frameworks | Ayni AgentOS |
|---|---|---|
| **Execution Model** | Ad-hoc loops blocking on LLM & tool I/O | Single-owner Kernel with bounded `step()` execution and asynchronous Host/Provider I/O |
| **Authority & Safety** | Prompt-level guardrails; tools exposed globally | Local-first principal, Grant, and Scope enforcement; human-in-the-loop approval & instant revocation |
| **Side Effects & Recovery** | Blind retries or duplicated actions on failure | Durable commit barriers, idempotent commands, `RESULT_UNKNOWN` ledger (`QUERY` / `ACK`), no blind replay |
| **Memory & Privacy** | Shared global context & implicit export | Private-by-default agent memory, explicit share/export grants, provenance & deletion lineage |
| **Footprint & Portability** | Heavy runtime & unbounded heap allocations | Zero-exception, zero-RTTI C++17 Core; caller-owned aligned arena; portable across Linux, Sim & ESP32-S3 |
| **Capability Contracts** | Unstructured string/JSON tool wrappers | Typed, versioned contracts, permission-filtered catalog discovery, multi-instance bindings, unary/action/subscription lifecycles |

## Key Capabilities

- **Single-Owner Portable Kernel (`core/`)**
  One logical owner advances Agent, Goal, Task, Grant, Resource, Memory, and Recovery state through bounded `step` calls. Core never allocates heap memory on its own (`ayni_kernel_memory_requirements`), keeps all network, model, storage, and device I/O across asynchronous boundaries, and compiles cleanly with `-fno-exceptions -fno-rtti`.

- **Verifiable Typed Capabilities & Scoped Discovery**
  Capabilities declare bounded typed inputs, structured results, units, versions, and execution semantics. Trusted configuration registers capabilities at startup; Tasks discover and inspect only the catalog entries permitted to them and resolve implementations via explicit Bindings with per-instance resource mapping. **Discovering a capability never grants execution authority.**

- **Local-First Governance & Private Memory**
  Models propose tool calls and long-lived goals; the local Kernel enforces principals, Grants, argument scopes, runtime limits, and resource quotas. Missing authority cleanly parks work (`PROPOSED`) for explicit approval rather than failing silently. Memory is private by default with separate grants for reading, sharing, and remote export, plus deletion lineage. *(Current enforcement covers trusted built-in code through managed entry points; strong OS/hardware sandboxing of arbitrary native code remains future work.)*

- **Commit Barriers, Crash Recovery & Unresolved-Operation Ledger**
  Durable-acceptance replies and external side effects are gated by WAL commit barriers and dual-slot snapshots. Uncertain side effects enter `RESULT_UNKNOWN` and move into an independent `OperationLedger` when their Task retires — preventing unresolved external history from exhausting active work capacity — until resolved via read-only `QUERY` reconciliation or explicit human `ACK`. Recovery rebuilds consistent state without blindly replaying old actions.

- **Explicit Lifecycles, Streaming Control & Optional Physical Safety**
  Supports `unary` calls, cancellable long-running `action`s, and lease-renewed `subscription`s with bounded queues, deadlines, and backpressure. Persistent Goals and individual Tasks are managed independently. Software-only deployments require no hardware stack; when physical execution capabilities are enabled, local permit gates (`control/permit_gate/`), envelopes, interlocks, streaming `ControlLoop` shaping, parameterized reflex rules, and an independent local stop path govern execution.

- **Standalone Source SDK & OpenAI-Compatible Models**
  Embed Ayni via the C99 ABI (`include/ayni/ayni.h`) or the zero-exception C++17 facade (`include/ayni/ayni.hpp`). Includes a shared `ToolLoop`, an OpenAI-compatible Chat/Responses model Provider, and a portable source SDK verified in isolated read-only builds.

## Quick Start

Try Ayni locally on Linux with the built-in **Mock model** — **no API key or hardware required**. See the [development guide](docs/development.md) for verified toolchains, dependency versions, offline configuration, and sanitizer presets.

### 1. Build and launch the Native CLI

Install a C++17 compiler, CMake (≥ 3.22), Ninja, Python 3, libcurl, and SQLite development headers:

```sh
sudo apt-get install -y build-essential cmake ninja-build python3 git \
    libcurl4-openssl-dev libsqlite3-dev

git clone https://github.com/ayni-org/ayni-os.git
cd ayni-os

# Inspect available memory, swap, CPU, and active jobs before choosing parallelism.
export CMAKE_BUILD_PARALLEL_LEVEL=2
export CTEST_PARALLEL_LEVEL=2

cmake --preset native-debug
cmake --build --preset native-debug --target ayni
./build/native/targets/native_cli/ayni --mock
```

The first configure downloads pinned dependencies (GoogleTest, yyjson; see [offline setup](docs/development.md#离线配置-googletest)). The CLI stores state in private per-user XDG directories (`~/.config/ayni`, `~/.local/share/ayni`).

### 2. Run a governed conversation

A fresh installation starts with **zero Grants** (default-deny). In the CLI, explicitly grant model and summary authority for the Mock conversation, then chat:

```text
/grant model
/wait
/grant summary
/wait
Hello
/wait
/conversation
/quit
```

`/wait` waits for actionable work and durable saves to finish; `/quit` performs a clean shutdown. You can also send a message first, inspect pending approvals with `/goals`, and authorize them with `/approve <goal-id>`.

<details>
<summary><strong>Connect a real OpenAI-compatible model (optional)</strong></summary>

Create `~/.config/ayni/config` (secrets are never stored in config files — pass only the environment variable name):

```ini
provider=openai
base_url=https://api.deepseek.com
model=deepseek-chat
key_environment=AYNI_LLM_API_KEY
```

Export your key and launch without `--mock`:

```sh
export AYNI_LLM_API_KEY="your-api-key"
./build/native/targets/native_cli/ayni
```

See the [Native CLI guide](targets/native_cli/README.md) for context-window limits, timezone rules, memory commands (`/memory`), reminders (`/remind`), deterministic `/stop`, and `/unresolved` ledger commands.

</details>

### 3. Run the public SDK examples

```sh
cmake --build --preset native-debug --target \
    ayni_example_new_hardware ayni_example_new_model \
    ayni_example_new_strategy ayni_example_composite_workflow \
    ayni_example_managed_operations
ctest --preset native-debug -R '^Examples\.'
```

All five examples use only public headers in `include/ayni/` and require no external credentials or physical devices:

| Example Target | What It Demonstrates |
|---|---|
| `ayni_example_new_hardware` | Simulated tactile-button `SubscriptionProvider` + rule (start ACK, samples, lease renew/cancel, stop confirmation) |
| `ayni_example_new_model` | Second OpenAI-compatible model over local loopback HTTP with independent Binding, destination, and explicit Grant |
| `ayni_example_new_strategy` | Custom `LocalInput` rule strategy completing a durable Task without modifying Core |
| `ayni_example_composite_workflow` | End-to-end capture → model → stop → presentation workflow with provenance checks |
| `ayni_example_managed_operations` | Concurrent cancellable chunked-sum `action` and I64 event `subscription` on the same Task across Sim & Native storage |

<details>
<summary><strong>Run the simulator and standalone SDK portability check</strong></summary>

```sh
# Build and run the lifecycle simulator
cmake --build --preset native-debug --target ayni-sim
./build/native/targets/simulator/ayni-sim

# Run the isolated read-only source SDK consumer (no SQLite/libcurl required)
python3 tests/portability/run_native.py --source . --source-revision "$(git rev-parse HEAD)"
```

See [public SDK examples](examples/README.md) and [independent integration guide](tests/portability/README.md) for details.

</details>

## Architecture

![Ayni architecture](docs/assets/ayni-architecture.svg)

*The diagram includes planned extensions; public contracts and delivery reports define the current implementation and validation scope.*

### Layer Responsibilities

| Layer | Responsibility |
|---|---|
| **Applications & Strategies** | Assistants, software workflows, goal types, rules, and `ToolLoop` define product behavior and advance work |
| **C99 API / C++17 SDK** | Submit typed commands, query snapshots, and register capabilities with unified authorization, error, and lifecycle semantics |
| **Shared Kernel (`core/`)** | Eight invariant-owning modules with strict acyclic dependency direction (see below) |
| **Hosts & Providers** | Hosts supply platform services (clock, durable storage, lifecycle); Providers implement models, software jobs, event streams, or device calls |
| **Local Control & Stopping** | Validates permits, limits, and interlocks for enabled physical capabilities with an independent stop path (physical guarantees require hardware evidence) |
| **Host OS & Drivers** | Handles thread scheduling, system I/O, and concrete hardware access |

### Core Dependency Direction

Core stays completely platform-independent and enforces strict acyclic dependencies via `tools/check_deps.py` and CMake layer checks (`tools/architecture.json`):

```text
kernel ──▶ work ──▶ { authority, resources, knowledge } ──▶ catalog ──▶ durability ──▶ foundation
```

- **`runtime/` is the sole composition root:** it selects deployment profiles and wires concrete Hosts, Providers, strategies, and applications. Business logic, network stacks, model SDKs, and hardware drivers stay strictly outside Core.
- **Clear abstraction roles:** a **Skill** expresses a reusable workflow, a **Capability** declares an execution contract, a **Provider** implements capabilities, and a **Driver** talks to hardware.
- **Independent versioning:** C ABI (`0.62`), wire/management protocols, and Core storage schema (`12`) evolve independently. ABI `0.x` minor versions require an exact match; storage rejects unsupported older schemas while preserving existing directories (see [development guide](docs/development.md)).

## Project Layout

```text
core/              Portable C++17 Kernel, split into 8 invariant-owning modules
include/ayni/      Public C99 ABI headers, C++17 lifecycle wrapper, and draft headers
contracts/         Versioned domain/host/wire/storage contracts and traceability matrix
strategies/        Rules, ToolLoop, and work-advancement policies
applications/      Assistant coordinator, software workflows, and fixture applications
hosts/             Native, Sim, and ESP-IDF platform services plus shared storage engine
providers/         OpenAI-compatible model, software jobs, Shell, presentation, and mocks
runtime/           Composition root: deployment selection, shared conversation & operation sessions
control/           Permit-checked local execution gate (control/permit_gate; safety/ planned)
targets/           Native CLI (ayni), simulator (ayni-sim), and ESP32 diagnostic entry points
experiments/       Independent platform and application experiments (e.g. ESP32-S3 Wi-Fi/voice)
examples/          Five standalone public SDK integration examples
tests/             Unit, contract, integration, fault-injection, and portability tests
docs/, reports/    Architecture plans, ADRs, development guides, and validation evidence
```

## Implementation and Roadmap

Ayni follows a **foundation-first** roadmap ([ADR-0083](docs/adr/0083-general-foundation-before-platform-experiments.md)): complete and validate the general-purpose AgentOS foundation before platform-specific experiments. Platform experiments are independent — Linux system integration does not depend on ESP32 or ROS.

| Scope | Status & Validation Evidence |
|---|---|
| **Native Core, Simulator & C/C++ SDK** | **Delivered** — Historical E1–E4 Native/Sim fault-injection & recovery evidence in [Core Alpha Native report](reports/core/m1-core-alpha.md) |
| **General Capability Foundation** | **Delivered** — Typed inputs/results, scoped discovery, multi-instance resource bindings, managed long operations & subscriptions; see [foundation report](reports/core/foundation-acceptance.md) |
| **Standalone Source SDK & Shared App** | **Delivered** — Read-only isolated import, shared Native program & minimal normal reopen pass; ESP32-S3 standalone compilation/linking pass; see [independent integration report](reports/core/independent-consumption.md) |
| **Low-Latency Control & Unresolved Ledger** | **Implemented (ABI 0.62 / Schema 12)** — Independent `OperationLedger` (`QUERY`/`ACK`/post-durability audit), async UI, streaming `ControlLoop` & parameterized reflex rules; portable software checks & ESP firmware build pass ([ADR-0114](docs/adr/0114-async-ui-streaming-control-and-parameterized-reflex.md), [ADR-0115](docs/adr/0115-unknown-operation-history-and-work-retirement.md)) |
| **ESP32-S3 Touchscreen Wi-Fi/Voice** | **Independent Experiment** — Implemented in `experiments/esp32_wifi_chat/`; full 128K context peak, 60-second motor action, on-board latency & general hardware stop-chain/endurance remain unaccepted ([experiment guide](experiments/esp32_wifi_chat/README.md)) |
| **Further Platforms & Extensions** | **Planned** — Linux OS service integration, vision, production OTA, MCP / ROS 2 / MHS adapters, peer multi-node (`MA-038`); each gated on its own entry criteria ([implementation plan](docs/ayni_v1.0.md)) |

Native/Sim runs, firmware compilation, on-board functionality, and endurance results are always reported separately. See [M1 progress & handoff](reports/core/m1-progress.md) for the latest detailed status.

> [!IMPORTANT]
> **Public Repository History Note ([ADR-0116](docs/adr/0116-apache-license-and-public-copy.md)):** This public repository (`ayni-org/ayni-os`) preserves the original project's complete Git commit history. Historical PR and CI links pointing to `RobotSe7en/ayni` remain in the original private repository and require access to it; those platform records were not transferred by the Git push.

## Documentation

- **[Development Guide](docs/development.md)** — Verified toolchains, pinned dependencies, CMake presets, sanitizers, and CI setup.
- **[Native CLI Guide](targets/native_cli/README.md)** — Interactive chat, Grants, OpenAI-compatible config, private memory, reminders, and `/unresolved` tools.
- **[Public SDK Examples](examples/README.md)** & **[Independent Integration](tests/portability/README.md)** — Extending capabilities, swapping Providers, and embedding the source SDK.
- **[Public Contracts](contracts/README.md)** & **[Test Traceability](contracts/traceability.md)** — Clause-level guarantees and mapped tests.
- **[Foundation & Experiment Plan](docs/architecture/foundation-and-experiment-plan.md)**, **[Full Implementation Plan](docs/ayni_v1.0.md)**, and **[ADR Index](docs/adr/README.md)** — Design rationale, roadmap order, and architectural decisions.
- **[Logo Assets](docs/assets/ayni-logo.svg)** — [Inverse SVG](docs/assets/ayni-logo-inverse.svg) · [Opaque PNG](docs/assets/ayni-logo.png).

*Note: Most detailed internal design documents, contracts, and ADRs are currently written in Chinese.*

## Contributing and Acknowledgements

Before contributing, please read **[AGENTS.md](AGENTS.md)** and the **[development workflow](docs/workflow.md)**.

- Contract, ABI, or compatibility changes must update the corresponding ADRs, tests, and documentation.
- Never commit credentials, API keys, or private media; supply secrets via runtime configuration.
- Run `git diff --check`, `python3 tools/check_deps.py`, and `python3 tools/check_contracts.py` before opening a pull request.

README structure was inspired by [dagent](https://github.com/RobotSe7en/dagent). The architecture diagram's layered visual presentation references [AIOS](https://github.com/agiresearch/AIOS), while all components, invariants, and contracts follow Ayni's own design. Additional research references appear in the implementation plan appendix. Citations acknowledge prior research without implying integration, compatibility, or upstream endorsement.

## License

Copyright 2026 Ayni contributors.

Except for third-party components and assets with their own notices, this project's code and documentation are licensed under the **[Apache License 2.0](LICENSE)**. See [NOTICE](NOTICE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for attribution and third-party license details.
