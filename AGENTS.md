# Repository Guidelines

## Project Structure & Module Organization

Implementation is currently limited to `docs/ayni_v1.0.md`, the Ayni AgentOS plan. Read it before contributing; APIs, directories, and platform support remain proposed. Preserve its filename when updating the internal document version.

Follow the planned layout when implementation begins:

- `core/`: portable Kernel state, scheduling, authorization, and recovery.
- `include/ayni/`, `contracts/`: public C/C++ interfaces and versioned contracts.
- `hosts/`, `providers/`, `drivers/esp32/`: platform services, asynchronous I/O, and hardware drivers.
- `apps/`, `ui/web/`: simulator, ESP32 assistant, and management UI/assets.
- `tests/`: unit, contract, integration, fault-injection, fuzz, and hardware-in-loop tests.
- `docs/adr/`, `reports/`: architectural decisions and validation evidence.

## Build, Test, and Development Commands

No build scripts or runnable application exist yet. Planned native commands:

```sh
cmake -S . -B build/native -DCMAKE_BUILD_TYPE=Debug -DAYNI_BUILD_TESTS=ON -DAYNI_HOST=native
cmake --build build/native -j
ctest --test-dir build/native --output-on-failure
./build/native/apps/simulator/ayni-sim
```

These configure, build, test, and launch the simulator. Future firmware builds use `idf.py build` from `apps/esp32_assistant/` after activating the pinned ESP-IDF environment.

## Coding Style & Naming Conventions

Target C++17 Core and a C99-compatible public API. Keep STL types, exceptions, and OS handles outside the ABI; disable exceptions/RTTI in the C++ wrapper. Use bounded storage and explicit allocation failures.

No formatter configuration exists. For new C/C++ files, use four-space indentation, `snake_case` functions/files, and the planned `ayni_` public-symbol prefix. Preserve the Chinese prose and heading structure in the implementation plan. Planned checks include formatting, compiler warnings, clang-tidy, and ASan/UBSan; add configurations before claiming enforcement.

## Testing Guidelines

GoogleTest is a candidate, not an installed framework. Use descriptive scenario names such as `recovery_after_dispatch_barrier`. Plan deterministic fixtures for multi-Agent resource sharing, crash recovery, and model/strategy replacement. Target Core coverage ≥75% and safety-critical branch coverage ≥90%; record deviations with evidence and an ADR. Distinguish compilation, simulation, hardware, and endurance results.

## Commit & Pull Request Guidelines

Use concise, imperative, scoped commit messages, for example `docs: clarify recovery guarantees`. Keep the initial history focused on documentation and repository setup. PRs should explain behavior, link relevant issues/plan sections, and report validation and limitations. Include screenshots for UI changes and traces for recovery changes. Update contracts, migrations, and ADRs when semantics change.

## Architecture & Security Constraints

Keep Kernel state under one logical owner and I/O asynchronous. Never blindly retry `RESULT_UNKNOWN` actions. Keep credentials and private media out of commits and logs; configure secrets at runtime. Mark capabilities implemented only with corresponding validation evidence.
