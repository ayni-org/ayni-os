# Ayni

[中文](README.md) · English

**A cross-device personal assistant AgentOS: one assistant for the user, with multiple Agents sharing managed system services.**

> This repository is at M1. The Core implements the work model, triggers and strategies, journal/snapshot recovery, and MA-006 A–F: trusted binding, Capability/Scope, Grants/delegation, atomic approval, independent Goal/Task authorization, revocation cleanup, explicit reauthorization and normal recovery of complete new-format work (merged baseline ABI 0.28, storage schema 2). See [M1 progress](reports/core/m1-progress.md) for current limits and next steps, and [ADR-0038](docs/adr/0038-work-authority-and-explicit-reauthorization.md) for validation and this round's coverage threshold. This branch adds resource registration, health queries and bounded fair admission in ABI 0.29; Task runtime limits and the Provider call path are still outstanding (ADR-0042). Native and simulated Hosts provide storage. Firmware compilation and hardware acceptance are distinct; ESP-IDF storage, real permits/Providers, complete E1–E4 experiments, an SDK and hardware acceptance remain outstanding. The rest of this README describes intended behavior; build instructions are in the [development guide](docs/development.md) (Chinese).

See the [Ayni implementation plan](docs/ayni_v1.0.md) (Chinese) for the complete design, scope, draft interfaces, and acceptance gates.

## Architecture

Ayni defines its architecture around its own task, authorization, resource, and recovery requirements. It plans a portable C++17 Core for desktops and constrained devices, with a C99-compatible API and a C++ wrapper. The first hardware validation target is an existing ESP32-S3 board; its exact model and capacities still require measurement.

Ayni's domain contracts govern tasks, authorization, and execution; external ROS 2, MHS, and MCP protocols connect through adapters, which normally absorb field changes, while new execution semantics require versioned evolution and may change Core.

![Ayni architecture](docs/assets/ayni-architecture.svg)

| Layer | Responsibilities and function abstractions |
|---|---|
| Applications, Coordinator, strategies, and Skills | Define reminders and observation workflows, route user requests, select Agents, and compose bounded work; rules, Tool Loops, and planners remain above the Kernel |
| C API / SDK | Submit typed intents or commands, query snapshots, and register goal types and capabilities, while preserving common authorization, error, and lifecycle semantics |
| Shared Kernel services | Nine modules split by invariant owner, with entity-named subdirectories: `kernel` (entry, orchestration, and output gating), `work` (Agents, Goals, Tasks, calls, and the action ledger), `authority` (principals, Grants, and Scenes), `resources` (admission, leases, and permits), `budget` (budget hierarchy, reservation, and settlement), `knowledge` (observations, memory, and context), `catalog`, `durability`, and `foundation` |
| Async execution and platform services | Hosts supply clocks, storage, networking, and lifecycle services; Providers implement models, perception, media, and actions |
| Local control and stopping | Controllers execute within admitted envelopes under short, renewable permits, and executors can only tighten authority; a local stop supervisor operates independently of models and ordinary work queues, and the stop chain includes hardware disable |
| OS, drivers, and hardware | Reuse existing OS scheduling; Drivers access specific hardware; the clocks, interrupts, and hardware timeouts that the stop chain depends on require measurement |

A Skill is a reusable application workflow; a Capability is a system-validated execution contract; a Provider implements an execution interface; a Driver accesses hardware. New applications, models, and hardware should enter through registration and adapters. Changes to authorization, recovery, or resource semantics still require contract versions and tests.

## Core responsibilities

The Kernel owns the logical state of Agent instances, Goals/Tasks, authorization, action records, resources, and memory. Each device has one logical state owner, advanced through bounded events and `step` calls. Networking, Flash, model calls, and device operations use asynchronous interfaces.

Key design constraints include:

- Define consistent commit and recovery boundaries across trigger cursors, Tasks, budgets, and action records. Durable-acceptance replies, external events, and side-effect dispatches are released only after their commit enters the durable prefix. On a storage write failure, the Kernel enters a restricted fence, stops new outputs and permit renewal, and reuses the restart recovery path.
- Persist a possible-dispatch barrier before requests with side effects. Uncertain outcomes enter `RESULT_UNKNOWN` for authoritative queries or human resolution; recovery must not blindly replay actions.
- Validate principals, Grants, arguments, resources, runtime limits, observation freshness, and deadlines. Models propose; the local Kernel controls execution.
- Actuators run only under short, actively renewed permits issued by the control domain's authority owner. Revocation, Kernel or executor restarts, calibration invalidation, emergency-stop reset, and network partitions all invalidate old permits; worst-case stop bounds are measured term by term.
- Bound media, queues, context, Tasks, and storage. Resource cancellation, timeout, and stopping require explicit release or quarantine states.

Business completion rules, planning, model inference, perception algorithms, codecs, TLS, and motor control belong in applications, Hosts, Providers, or Drivers. The first release uses trusted built-in code, with authorization guarantees at managed entry points. Isolation against arbitrary native code on ESP32 requires separate future validation. Model, VLA, and external-message outputs are untrusted data; pure-function strategy interfaces are not a security boundary; ROS, MHS, and MCP adapters run as permission-restricted processes, and executor-side permit checks are the final line of defense.

## Agreed product semantics

1. **Shared assistant profile:** Agents receive explicit access to a common assistant profile. Agent memory remains private by default; sharing requires authorization.
2. **One owner:** v0.1 has a single device owner. Push-button voice requests use existing authorization; sensitive operations such as adding permissions require confirmation through the bound management client.
3. **Pausing the assistant:** Accepted reminders remain active. New autonomous observation, reflection, and motion are paused, and related in-flight work is brought to a stop or checkpoint. Pause, cancellation, and emergency stop have distinct meanings.
4. **Forgetting:** Deleted memory cannot be used again. In-flight results and derived content that depend on it are invalidated. Data already sent to a model remains subject to the upstream retention policy.
5. **Reminder completion:** A reminder completes after successful presentation through the user's selected method, without requiring acknowledgement for ordinary reminders. Creating a notification record or queuing output alone does not count as success; failures remain pending.

v0.1 plans local-network management, push-button voice interaction, continuous visual capture with bounded analysis, and four built-in goal types: reminders, device checks, memory organization, and scene observation. Motion uses calibrated, bounded short actions; autonomous motion is limited to a user-enabled turn-to-observe scene. Additional hardware targets CNY 300 per device and may grow when needed, with the reason recorded; this excludes the existing board, model fees, and labor. Raw media is temporary by default. Mobile background push, autonomous navigation, isolation for arbitrary third-party plugins, and automatic cross-device task takeover are outside this release's commitment.

## Three versioned contracts

| Contract | Compatibility boundary |
|---|---|
| Core—Host / native SDK | ABI major/minor versions, structure size, and feature bits (0.x minor versions must match exactly); no STL types, exceptions, or OS handles across the ABI |
| Management and device protocols | Protocol major/minor versions, capability versions, and feature negotiation; shared domain semantics with authentication and local authorization checks |
| Persistent storage | Schema, record, and migration versions; migration, corruption recovery, and firmware rollback are validated together |

These contracts evolve independently and remain aligned through domain definitions and conformance fixtures. The implemented C ABI is experimental version 0.19. D merged in #28; validation is recorded in ADR-0031. Unimplemented interfaces remain in draft headers. Message IDs, operation IDs, and local request epochs serve different purposes, and each message kind has exactly one idempotency key. Node authentication must be complemented by evidence binding the node to the calling principal. The protocol makes no universal exactly-once execution guarantee.

## Optional embodied profiles and adapters

Execution permits, control sessions, stop bounds, and motion-outcome phases are base contracts shared by every actuator, and the v0.1 wheeled motion uses them directly. The optional `EMBODIMENT_V1` profile only adds robot-specific types for space and time, state, trajectories, and model candidates, negotiated by feature and governed by the three versioned contracts. Ordinary devices need neither robot-specific fields nor a ROS dependency.

| Contract | Planned boundary |
|---|---|
| Observations and robot state | Specify frames, clock domains/epochs, units, calibration versions, and `RobotState`; tf2, URDF, perception, and coordinate transforms remain in Providers or bridges |
| Control authority and local execution | `ControlAuthority` declares control modes and command interfaces on top of the base execution permit; epochs, command sequences, and maximum command age are checked by the executor, local control sessions are bounded and subject to interlocks, and local controllers run real-time loops |
| Motion outcomes and models | The accepted, executed, stop-confirmed, and goal-verification phases of `MotionOutcome` are base contracts, and the profile adds pose and joint verification evidence; VLA outputs are candidates that can execute only inside an admitted control session after executor permit and envelope checks |

The ROS 2 Bridge remains outside Core and maps Topics/Services/Actions to `subscription/unary/action`. Imports and exports use existing managed entry points. The Bridge records mappings between ROS goal UUIDs and Ayni call identities; ROS acceptance, cancellation responses, and QoS do not themselves provide durable deduplication, authorization, or physical-stop guarantees. ROS 2 DDS-Security provides authentication and topic access control, and the Bridge must enable its strict mode; it complements rather than replaces Ayni Grants, budgets, and recovery semantics. Ayni relates to external ecosystems through versioned mappings with explicitly declared supported subsets. M3 first validates a Linux Native Kernel and ROS simulation nodes on the same computer. PC/ESP32 paths wait for M5; MCU micro-ROS requires later capacity, concurrency, and disconnection testing.

As of 2026-10-18, MHS (Model Hardware Standard) remains a research preview; see the [official introduction](https://www.anthropic.com/news/model-hardware-standard-research-preview) and [preview portal](https://www.modelhardwarestandard.com/). Adaptation requires a specific, actionable specification, lawful access, licensing, and test fixtures; any SDK used must have a pinned version. Current compatibility is not claimed. Import/export must use the same authorization, schema validation, and execution entry points. Arbitrary generated code must not be deployed directly to the MCU, and MHS code files are accepted only for the subset convertible into bounded Procedure data.

## Roadmap and validation

| Stage | Target and gate |
|---|---|
| M0 | Contracts (including output gating, execution permits, cross-device identity, and idempotency), project skeletons, pinned tooling, board/BOM feasibility including stop-chain hardware, and an optional embodied-profile draft |
| M1 · Core Alpha | Native Core, simulator, and SDK; E1–E4 pass on desktop and reproduce in Linux CI; deterministic multi-node simulation (does not block M2, required before M5); optional Mock conformance R1.a |
| M2 · Core Beta | The same Core, E1–E4, and hardware/model/strategy/composite-workflow extension examples validated on ESP32 |
| M3 | Camera, push-button voice, chassis, independent stopping, and authorized turning; real concurrency and cost validation; optional Linux ROS 2 Bridge/simulation R1.b |
| M4 · v0.1 | Provisioning, binding, management UX, gateway, and controlled user trials; real OTA/rollback and at least 72 hours of integrated hardware endurance testing |
| M5 · v0.2 | Independent PC and ESP32 instances, authentication, remote calls, deduplication, queries, and disconnection recovery |
| M6 | Separately gated hosts, durable subtask delegation, MCP/A2A adapters, more sensors and mechanisms, third-party isolation, and conditional MHS adaptation R1.c |

E1 checks multiple Agents sharing system services. E2 checks restarts at critical action stages. E3 checks replacing a model or compatible strategy after restart. E4 checks bounded composition, subscription dependencies, and semantic upgrades/rollback. It covers invalidating in-flight dependencies, step recovery, pausing on incompatible versions, and simulated storage migration/rollback; real OTA is validated in M4. Experiments pass in a deterministic simulator first, then on ESP32, followed by real hardware validation.

Planned extension examples include `new_hardware`, `new_model`, `new_strategy`, and `composite_workflow`. A new composite application must work through public interfaces without changing Kernel source.

R1 is a separate optional conformance suite; E1–E4 remain the four mandatory experiments. R1.a checks Mock profile contracts. R1.b validates the Linux Bridge/simulation after M2 Core Beta. R1.c depends on MHS specifications, access, licensing, and test fixtures. These optional routes do not block v0.1. Claims of profile, ROS Bridge, or MHS support require the applicable R1.a, R1.a/b, or R1.a/c gates respectively, without exemption.

Build, simulation, hardware functionality, and endurance results are reported separately. Linux Native M0/M1 builds, unit and contract tests, and simulated recovery checks have results; see [M1 progress](reports/core/m1-progress.md) for the actual configurations and evidence ([M0 baseline](reports/core/m0-review.md)). Complete experiment, hardware, and endurance results remain outstanding; commands, directories, and APIs in the plan that are not marked as implemented remain proposed.

## Contributing and acknowledgements

Read [AGENTS.md](AGENTS.md) and the [implementation plan](docs/ayni_v1.0.md) before contributing. Contract, migration, and material semantic changes need corresponding ADRs (see [docs/adr/](docs/adr/README.md)) and validation evidence. Code, logs, and public firmware must not contain private media, vendor API keys, or reusable device credentials.

The architecture diagram's visual presentation references [AIOS](https://github.com/agiresearch/AIOS); its components, layers, and contracts follow Ayni's own design. Thanks to the durable execution, device, protocol, and embedded projects listed in the plan's appendix. References acknowledge research; they do not imply integration, compatibility, or upstream endorsement. A project license has not yet been selected. Dependency selection and any source reuse require review of the relevant licenses.
