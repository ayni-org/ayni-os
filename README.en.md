<p align="center">
  <img src="docs/assets/ayni-logo.png" alt="Ayni" width="420">
</p>

<p align="center">
  <strong>Shared systems. Capabilities on demand.</strong>
</p>

<p align="center">
  <a href="https://github.com/ayni-org/ayni-os/actions/workflows/ci.yml"><img alt="CI" src="https://github.com/ayni-org/ayni-os/actions/workflows/ci.yml/badge.svg"></a>
  <a href="docs/development.md"><img alt="C++17" src="https://img.shields.io/badge/Core-C%2B%2B17-blue"></a>
  <a href="include/ayni/ayni.h"><img alt="C99 API" src="https://img.shields.io/badge/API-C99-blue"></a>
</p>

<p align="center">
  <a href="#quick-start">Quick Start</a> |
  <a href="#architecture">Architecture</a> |
  <a href="#documentation">Documentation</a> |
  <a href="examples/README.md">Examples</a> |
  <a href="README.md">中文</a>
</p>

**Ayni** is a general-purpose native AgentOS. Running on the host operating system, it gives multiple Agents shared task, authorization, resource, call, memory, and recovery mechanisms, with public contracts for software, model services, and device capabilities.

A portable C++17 Core manages system state, rules and ToolLoop strategies advance work, and Hosts and Providers supply asynchronous services. The desktop assistant and ESP32-S3 touchscreen experiment reuse this foundation; each deployment selects its applications, models, and peripherals.

> **Current status:** The general foundation and standalone source SDK are delivered. Native/Sim software evidence is indexed in the [foundation report](reports/core/foundation-acceptance.md) and [independent integration report](reports/core/independent-consumption.md). The current experimental C ABI is **0.62**, and the Core storage schema is **12** ([ADR-0115](docs/adr/0115-unknown-operation-history-and-work-retirement.md) covers the independent unresolved-operation ledger, QUERY/ACK, and post-durability audit; [ADR-0114](docs/adr/0114-async-ui-streaming-control-and-parameterized-reflex.md) covers the asynchronous UI, independent operations, and parameterized reflex rules). The ESP32 touchscreen Wi-Fi/voice experiment proceeds separately; full hardware stop-chain and endurance acceptance remain incomplete.

This public repository preserves the original project's commit history. Historical PR and CI links to `RobotSe7en/ayni` still refer to the original private repository and require access to it; those records were not copied by the Git push. See [ADR-0116](docs/adr/0116-apache-license-and-public-copy.md).

## Core Ideas

**1. Multiple Agents share system mechanisms.**
The Kernel owns Agent, Goal, Task, authorization, resource, and recovery state. One logical owner advances it through bounded `step` calls; models, networking, storage, and device I/O execute across asynchronous boundaries.

**2. Capabilities have verifiable contracts.**
Capabilities declare typed inputs, structured results, versions, and execution properties. Trusted configuration registers them. Tasks discover and inspect the catalog within their permissions, then select implementations through Bindings; discovery grants no execution authority.

**3. Execution authority stays local.**
Models propose calls; the Kernel checks principals, Grants, arguments, and resources. Memory is private by default, with explicit authorization for sharing and export. Current permission guarantees cover managed entry points used by trusted built-in code; strong isolation against arbitrary native code remains future work.

**4. Durable acceptance and side effects have explicit boundaries.**
Commit barriers gate durable-acceptance replies and side-effect dispatch. Uncertain results enter `RESULT_UNKNOWN` for authoritative queries or human resolution. Recovery rebuilds state without blindly replaying old actions.

**5. Capacity and lifecycle are explicit.**
Queues, Tasks, context, and storage have capacity limits. Calls support unary, action, and subscription lifecycles with cancellation, deadlines, backpressure, and release rules. Persistent Goals and individual Tasks are managed separately.

**6. One Core supports deployment-specific capabilities.**
The C99 API, C++17 wrapper, and source SDK support independent consumers. `runtime/` selects and composes Hosts, Providers, and applications. Software-only deployments are valid; physical execution additionally requires permits, interlocks, and an independent stop chain.

## Quick Start

Use the Linux Native development environment and Mock model below; no API key is needed. See the [development guide](docs/development.md) for verified toolchains, dependency versions, and other build options.

### Build and launch

Install a C++17 toolchain, CMake, Ninja, Python, and development packages for libcurl and SQLite:

```sh
sudo apt-get install -y build-essential cmake ninja-build python3 git \
    libcurl4-openssl-dev libsqlite3-dev

git clone https://github.com/ayni-org/ayni-os.git
cd ayni-os

# Check available memory, swap, CPUs, and other jobs; adjust to the resource budget.
export CMAKE_BUILD_PARALLEL_LEVEL=2
export CTEST_PARALLEL_LEVEL=2

cmake --preset native-debug
cmake --build --preset native-debug --target ayni
./build/native/targets/native_cli/ayni --mock
```

The first configuration downloads pinned dependencies; offline setup is documented in the development guide. The CLI defaults to private per-user configuration and data directories.

### Complete a conversation

A fresh installation has no Grants. In the CLI, authorize model calls and summaries for the Mock conversation, then enter a message:

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

`/wait` waits for currently actionable work and saves; `/quit` closes normally. Real models connect through the OpenAI-compatible Provider. Configuration, environment-based credentials, permissions, and other commands are documented in the [CLI guide](targets/native_cli/README.md).

### Run public examples

```sh
cmake --build --preset native-debug --target \
    ayni_example_new_hardware ayni_example_new_model \
    ayni_example_new_strategy ayni_example_composite_workflow \
    ayni_example_managed_operations
ctest --preset native-debug -R '^Examples\.'
```

Examples cover simulated hardware, a loopback model service, a new strategy, a composite workflow, and managed long operations. They need no real-model credentials or devices. See the [public SDK examples](examples/README.md).

## Architecture

![Ayni architecture](docs/assets/ayni-architecture.svg)

The diagram includes planned extensions; public contracts and delivery reports define implementation and validation scope.

| Layer | Responsibility |
|---|---|
| Applications and strategies | Assistants, software workflows, goal types, rules, and ToolLoop define business behavior and advance work |
| C API / C++ SDK | Submit typed commands, query snapshots, and register capabilities with common authorization, error, and lifecycle semantics |
| Shared Kernel | `kernel`, `work`, `authority`, `resources`, `knowledge`, `catalog`, `durability`, and `foundation` each own their invariants |
| Hosts and Providers | Hosts supply platform services such as clocks, storage, and lifecycle; Providers implement models, software jobs, event streams, or device calls |
| Local control and stopping | Check permits, limits, and interlocks for enabled physical capabilities, with independent stopping; physical guarantees require hardware evidence |
| Host OS and drivers | Schedule threads and implement system I/O and hardware access |

`runtime/` is the composition root: it selects deployments and assembles concrete components. Core stays platform independent; business policies, networking, model SDKs, and drivers remain at the boundaries. A Skill expresses a reusable workflow, a Capability declares an execution contract, a Provider implements capabilities, and a Driver accesses hardware.

ABI, management/device protocols, and storage schemas evolve independently. ABI 0.x minor versions must match exactly. Current storage rejects unsupported older schemas and preserves their directories; see the [development guide](docs/development.md) for migration and rollback boundaries.

## Project Layout

```text
core/              portable Kernel, split by state and invariant owner
include/ayni/      public C API, C++ wrapper, and draft interfaces
contracts/         versioned contracts and test traceability
strategies/        rules, ToolLoop, and other work-advancement policies
applications/      assistant, software workflows, and validation applications
hosts/             Native, Sim, ESP-IDF services, and shared storage
providers/         models, software jobs, Shell, presentation, and other capabilities
runtime/           deployment selection, composition, and shared runtime
control/, safety/  permit-checked execution and independent stopping
targets/           Native CLI, simulator, and firmware entry points
experiments/       independent platform and application experiments
examples/          public SDK integration examples
tests/             unit, contract, integration, and independent-consumer validation
docs/, reports/    designs, ADRs, development guides, and validation evidence
```

## Implementation and Roadmap

| Scope | Status and evidence |
|---|---|
| Native Core, simulator, C/C++ SDK | Delivered; historical E1–E4 evidence is indexed in the [Core Alpha Native report](reports/core/m1-core-alpha.md) |
| General capability foundation | Typed inputs/results, discovery and binding, instance resource mapping, long operations, and subscriptions are delivered; see the [foundation report](reports/core/foundation-acceptance.md) |
| Standalone source SDK and shared application | Delivered; read-only isolated import, the Native shared program, and minimal normal reopening pass; ESP32-S3 standalone compilation/linking pass; see the [integration report](reports/core/independent-consumption.md) |
| ESP32 touchscreen Wi-Fi/voice | Implemented as an independent experiment; full 128K context peak, 60-second motor actions, and the general hardware stop chain remain unaccepted; see the [experiment guide](experiments/esp32_wifi_chat/README.md) |
| Further platforms and extensions | Linux system integration, vision, OTA, MCP/ROS/MHS, peer operation, and full MA-038 have separate entry conditions; see the [implementation plan](docs/ayni_v1.0.md) |

Native/Sim, firmware compilation, hardware functionality, and endurance results are reported separately. Platform experiments independently select their capabilities; Linux requires neither ESP32 nor ROS. See [M1 progress](reports/core/m1-progress.md) for the latest handoff.

## Documentation

- [Development guide](docs/development.md): toolchains, dependencies, builds, tests, and CI.
- [Native CLI](targets/native_cli/README.md): conversation, permissions, models, memory, and reminders.
- [Public examples](examples/README.md) and [independent integration](tests/portability/README.md): extend capabilities, replace implementations, and reuse the SDK.
- [Public contracts](contracts/README.md) and [test traceability](contracts/traceability.md): interface guarantees and validation scope.
- [Foundation and experiment plan](docs/architecture/foundation-and-experiment-plan.md), [full implementation plan](docs/ayni_v1.0.md), and [ADRs](docs/adr/README.md): design, sequence, and decisions.
- [Logo assets](docs/assets/ayni-logo.svg): [inverse SVG](docs/assets/ayni-logo-inverse.svg) · [original PNG](docs/assets/ayni-logo.png).

Most detailed project documentation is in Chinese.

## Contributing and Acknowledgements

Read [AGENTS.md](AGENTS.md) and the [development workflow](docs/workflow.md) before contributing. Contract or compatibility changes should update the relevant ADRs, tests, and documentation. Supply credentials at runtime.

README organization follows [dagent](https://github.com/RobotSe7en/dagent). The architecture diagram's visual presentation references [AIOS](https://github.com/agiresearch/AIOS); components and contracts follow Ayni's own design. Further research sources appear in the implementation plan appendix. References acknowledge research, without implying integration, compatibility, or upstream endorsement.

## License

Copyright 2026 Ayni contributors.

Except for third-party components and assets with their own notices, this project's code and documentation are licensed under the [Apache License 2.0](LICENSE). See [NOTICE](NOTICE) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for attribution and third-party licenses.
