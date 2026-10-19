# 契约追踪矩阵

本文件由 `tools/check_contracts.py --write` 根据各契约文档生成，请勿手工编辑。修改条款后重新生成；本文件过期时 CTest 用例 `check_contracts_repository` 失败。

## 概览

| 文档 | 条款数 | M0 已实现 |
|---|---:|---:|
| [domain/authority_and_trust.md](domain/authority_and_trust.md) | 21 | 0 |
| [domain/calls_and_bindings.md](domain/calls_and_bindings.md) | 19 | 0 |
| [domain/commit_and_output.md](domain/commit_and_output.md) | 18 | 0 |
| [domain/ids_and_time.md](domain/ids_and_time.md) | 17 | 1 |
| [domain/lifecycles.md](domain/lifecycles.md) | 15 | 2 |
| [domain/limits.md](domain/limits.md) | 12 | 1 |
| [domain/permits_and_stop.md](domain/permits_and_stop.md) | 27 | 0 |
| [domain/primitives.md](domain/primitives.md) | 8 | 0 |
| [domain/product_semantics.md](domain/product_semantics.md) | 27 | 0 |
| [domain/resources_and_budget.md](domain/resources_and_budget.md) | 23 | 0 |
| [domain/status_codes.md](domain/status_codes.md) | 7 | 2 |
| [domain/versions.md](domain/versions.md) | 17 | 3 |
| [host/host_ports.md](host/host_ports.md) | 17 | 2 |
| [host/policy_interface.md](host/policy_interface.md) | 11 | 0 |
| [host/provider.md](host/provider.md) | 14 | 0 |
| [storage/records.md](storage/records.md) | 14 | 0 |
| [wire/envelope.md](wire/envelope.md) | 16 | 0 |
| [wire/json_subset.md](wire/json_subset.md) | 12 | 0 |
| 合计 | 295 | 11 |

## 计划章节 → 契约文档

| 计划章节 | 契约文档 |
|---|---|
| §0 | domain/product_semantics.md |
| §2.1 | host/host_ports.md |
| §2.2 | domain/authority_and_trust.md、domain/ids_and_time.md、domain/permits_and_stop.md、domain/primitives.md、domain/resources_and_budget.md |
| §2.3 | domain/primitives.md、domain/resources_and_budget.md |
| §2.4 | domain/commit_and_output.md、domain/lifecycles.md、domain/permits_and_stop.md、domain/product_semantics.md |
| §2.5 | domain/authority_and_trust.md、host/policy_interface.md |
| §2.7 | domain/calls_and_bindings.md、domain/ids_and_time.md、domain/limits.md、domain/primitives.md、host/policy_interface.md、host/provider.md |
| §2.8 | domain/commit_and_output.md、storage/records.md |
| §2.10 | domain/permits_and_stop.md |
| §3.1 | domain/versions.md、wire/envelope.md |
| §3.2 | domain/calls_and_bindings.md、domain/lifecycles.md、domain/limits.md、domain/status_codes.md、domain/versions.md、host/host_ports.md、host/provider.md |
| §3.3 | host/policy_interface.md、host/provider.md |
| §3.4 | domain/authority_and_trust.md、domain/ids_and_time.md、domain/limits.md、domain/status_codes.md、wire/envelope.md、wire/json_subset.md |
| §3.5 | domain/authority_and_trust.md、wire/envelope.md |
| §3.6 | domain/calls_and_bindings.md、domain/ids_and_time.md、domain/lifecycles.md、domain/primitives.md、host/provider.md、wire/envelope.md、wire/json_subset.md |
| §4.1 | domain/permits_and_stop.md、host/host_ports.md |
| §4.2 | domain/limits.md、host/host_ports.md、storage/records.md |
| §4.3 | domain/calls_and_bindings.md、domain/commit_and_output.md、domain/lifecycles.md、host/host_ports.md、storage/records.md |
| §4.4 | domain/ids_and_time.md、domain/lifecycles.md、domain/product_semantics.md、domain/versions.md、storage/records.md |
| §5.1 | domain/resources_and_budget.md、host/provider.md |
| §5.2 | domain/product_semantics.md |
| §5.3 | domain/limits.md、domain/resources_and_budget.md、host/policy_interface.md |
| §5.4 | domain/authority_and_trust.md、domain/product_semantics.md |
| §7.3 | domain/permits_and_stop.md |
| §10.6 | domain/limits.md、wire/json_subset.md |
| §10.7 | domain/product_semantics.md |
| §14.1 | domain/product_semantics.md |

## ADR → 契约文档

| ADR | 契约文档 |
|---|---|
| ADR-0001 | domain/permits_and_stop.md、domain/primitives.md、domain/resources_and_budget.md |
| ADR-0002 | domain/commit_and_output.md、host/host_ports.md、storage/records.md |
| ADR-0003 | domain/authority_and_trust.md |
| ADR-0004 | domain/authority_and_trust.md、domain/calls_and_bindings.md、domain/ids_and_time.md、domain/versions.md、wire/envelope.md |
| ADR-0005 | domain/permits_and_stop.md |
| ADR-0006 | domain/lifecycles.md、domain/status_codes.md、host/host_ports.md、host/policy_interface.md、host/provider.md |
| ADR-0007 | domain/calls_and_bindings.md、domain/lifecycles.md、domain/primitives.md、domain/product_semantics.md |
| ADR-0008 | domain/primitives.md、domain/resources_and_budget.md |
| ADR-0009 | domain/versions.md、wire/envelope.md、wire/json_subset.md |
| ADR-0013 | domain/versions.md |
| ADR-0014 | domain/limits.md、host/host_ports.md |

## 工作包 → 条款

| 工作包 | 条款 |
|---|---|
| `MA-001` | C-STOP-003、C-STOP-004 |
| `MA-003` | C-TIME-001、C-LIMIT-001、C-LIMIT-008、C-LIMIT-011、C-STOP-001、C-HOST-002、C-HOST-003、C-HOST-006、C-HOST-007 |
| `MA-004` | C-AUTH-004、C-ID-001、C-LIFE-020、C-LIFE-021、C-LIFE-022、C-LIMIT-007、C-POLICY-009 |
| `MA-005` | C-LIFE-002、C-LIFE-003、C-LIFE-005、C-LIFE-006、C-LIMIT-002、C-STATUS-001、C-STATUS-003、C-STATUS-006、C-HOST-015、C-PROV-001、C-WIRE-014、C-JSON-009、C-JSON-012 |
| `MA-006` | C-AUTH-001、C-AUTH-003、C-AUTH-008 |
| `MA-007` | C-HOST-002、C-HOST-007、C-HOST-008、C-HOST-015、C-WIRE-001、C-WIRE-002、C-WIRE-003、C-WIRE-013、C-WIRE-014、C-JSON-001、C-JSON-002、C-JSON-003、C-JSON-004、C-JSON-009 |
| `MA-008` | C-CALL-001、C-CALL-003、C-BIND-002、C-BIND-003、C-BUDGET-005、C-PROV-002、C-PROV-003、C-PROV-010 |
| `MA-009` | C-LIMIT-005、C-LIMIT-006、C-POLICY-001、C-POLICY-002、C-POLICY-005、C-POLICY-011 |
| `MA-010` | C-COMMIT-001、C-COMMIT-003、C-COMMIT-014、C-TIME-003、C-VER-020、C-VER-023、C-HOST-009、C-HOST-010、C-HOST-011、C-STORE-001、C-STORE-003、C-STORE-006、C-STORE-007、C-STORE-013、C-STORE-014 |
| `MA-011` | C-TIME-004、C-TIME-005、C-TIME-006、C-TIME-007、C-LIMIT-012、C-PROD-032、C-PROD-034、C-PROD-042、C-JSON-010 |
| `MA-012` | C-LIMIT-010 |
| `MA-015` | C-PRIM-005、C-PROV-013、C-PROV-014、C-WIRE-011 |
| `MA-016` | C-AUTH-002、C-AUTH-012、C-AUTH-015、C-LIFE-020、C-PRIM-007、C-VER-010、C-WIRE-003、C-WIRE-006、C-WIRE-007、C-WIRE-009、C-WIRE-010 |
| `MA-018` | C-AUTH-016、C-LIMIT-002、C-PERMIT-001、C-PERMIT-003、C-PERMIT-016、C-PRIM-001、C-PRIM-008、C-RES-001、C-RES-003、C-RES-006、C-RES-012、C-RES-013 |
| `MA-019` | C-PROD-012、C-PROD-015、C-PROD-050 |
| `MA-026` | C-AUTH-011、C-PERMIT-010、C-PERMIT-011、C-PERMIT-012、C-PERMIT-016、C-PERMIT-017、C-STOP-002、C-STOP-003、C-STOP-004、C-STOP-005、C-STOP-007、C-STOP-008、C-STOP-009、C-STOP-010、C-STORE-012 |
| `MA-027` | C-PROD-033、C-BUDGET-010、C-PROV-011 |
| `MA-028` | C-PROD-001、C-PROD-003、C-PROD-004、C-PROD-051 |
| `MA-030` | C-WIRE-015 |
| `MA-034` | C-TRUST-002 |
| `MA-035` | C-PERMIT-014、C-JSON-011 |
| `MA-037` | C-TRUST-005 |
| `MA-038` | C-AUTH-012、C-AUTH-013、C-ID-005、C-TIME-002、C-PERMIT-001、C-PERMIT-015、C-PERMIT-017、C-RES-012、C-WIRE-006、C-WIRE-008、C-WIRE-009、C-JSON-010 |

## 实验步骤 → 条款

| 实验步骤 | 条款 |
|---|---|
| E1 步骤 1 | C-PRIM-007、C-PROD-041、C-PROD-042、C-WIRE-010 |
| E1 步骤 2 | C-RES-009、C-RES-011、C-PROV-012 |
| E1 步骤 3 | C-RES-001、C-RES-002、C-RES-004、C-RES-006、C-RES-009、C-RES-010 |
| E1 步骤 4 | C-CALL-009、C-LIFE-012、C-STOP-006、C-PRIM-004、C-RES-007、C-RES-008、C-HOST-005、C-PROV-006 |
| E1 步骤 5 | C-AUTH-004、C-LIMIT-005、C-RES-003、C-BUDGET-001、C-BUDGET-003、C-BUDGET-004、C-BUDGET-007、C-BUDGET-008 |
| E1 步骤 6 | C-AUTH-001、C-AUTH-002、C-AUTH-004、C-AUTH-005、C-AUTH-011、C-LIFE-013 |
| E1 步骤 7 | C-AUTH-009、C-AUTH-010、C-TRUST-001、C-PROD-002、C-PROD-035、C-PROD-040、C-PROD-041 |
| E1 步骤 8 | C-SUB-001、C-SUB-002、C-SUB-003、C-LIMIT-009、C-PRIM-005、C-PROD-023、C-RES-010 |
| E1 步骤 9 | C-CALL-005、C-COMMIT-012、C-ID-001、C-PROD-030、C-PROD-031、C-BUDGET-009、C-PROV-003、C-PROV-007 |
| E1 步骤 10 | C-CALL-012、C-SUB-004、C-LIMIT-008、C-STOP-001、C-RES-004、C-RES-005、C-BUDGET-006、C-HOST-006、C-WIRE-013 |
| E1 步骤 11 | C-CALL-003、C-TIME-005、C-LIFE-023、C-STOP-002、C-PROD-010、C-PROD-012、C-PROD-013、C-PROD-014、C-PROD-016、C-PROD-020、C-PROD-021 |
| E2 F0 | C-STORE-002 |
| E2 F1 | C-AUTH-005、C-AUTH-007、C-TIME-003、C-PRIM-001、C-PRIM-006 |
| E2 F2 | C-COMMIT-006、C-PERMIT-002、C-PRIM-001 |
| E2 F3 | C-OUT-002、C-LIFE-010、C-PRIM-001、C-STATUS-004 |
| E2 F4 | C-LIFE-010、C-PRIM-001、C-STATUS-004 |
| E2 F5 | C-ID-004、C-LIFE-010、C-BUDGET-003、C-STATUS-004 |
| E2 F6 | C-COMMIT-004、C-WIRE-007 |
| E2 F7 | C-CALL-002、C-CALL-011、C-COMMIT-002、C-ID-006、C-HOST-003、C-HOST-009、C-HOST-011、C-STORE-001、C-STORE-002、C-STORE-009 |
| E2 F8 | C-COMMIT-002、C-COMMIT-005、C-LIMIT-007、C-BUDGET-002、C-HOST-009、C-STORE-002 |
| E2 F9 | C-COMMIT-004、C-ID-007、C-WIRE-004、C-WIRE-005、C-WIRE-016 |
| E2 F10 | C-AUTH-005、C-AUTH-006、C-COMMIT-007、C-OUT-003、C-LIFE-013、C-PERMIT-002、C-PRIM-006、C-HOST-004 |
| E2 F11 | C-CALL-012、C-COMMIT-005、C-BUDGET-002、C-STORE-007 |
| E2 F12 | C-COMMIT-008、C-COMMIT-009、C-COMMIT-010、C-COMMIT-011、C-ID-009、C-PERMIT-009、C-HOST-012、C-STORE-005 |
| E2 F13 | C-LIFE-004 |
| E2 F14 | C-TRUST-004、C-PERMIT-004、C-PERMIT-005、C-PERMIT-006、C-PERMIT-007、C-PERMIT-008、C-PERMIT-012、C-PERMIT-013 |
| E3 步骤 1 | C-POLICY-006、C-PROV-012 |
| E3 步骤 2 | C-CALL-002、C-CALL-008、C-CALL-011、C-ID-003、C-ID-004、C-ID-006、C-POLICY-003、C-POLICY-006、C-POLICY-010、C-PROV-012 |
| E3 步骤 3 | C-ID-003、C-POLICY-006、C-PROV-012 |
| E3 步骤 4 | C-CALL-004、C-ID-003、C-POLICY-006、C-POLICY-008、C-POLICY-010、C-PROV-012 |
| E3 步骤 5 | C-CALL-004、C-LIFE-024、C-VER-030、C-POLICY-006、C-POLICY-007、C-PROV-012 |
| E3 步骤 6 | C-PROV-011 |
| E4 步骤 1 | C-CALL-001、C-BIND-001、C-POLICY-003、C-PROV-009、C-JSON-012 |
| E4 步骤 2 | C-CALL-004、C-CALL-007、C-ID-003、C-POLICY-008 |
| E4 步骤 3 | C-CALL-006、C-CALL-008、C-CALL-009、C-LIFE-011、C-LIFE-022、C-LIMIT-006 |
| E4 步骤 4 | C-SUB-002、C-COMMIT-012、C-PROD-030 |
| E4 步骤 5 | C-VER-011、C-VER-030、C-POLICY-009 |
| E4 步骤 6 | C-VER-020、C-VER-021、C-VER-022、C-VER-031、C-STORE-004、C-STORE-010 |
| E4 步骤 7 | C-PROD-010、C-PROD-011、C-PROD-013、C-PROD-015、C-PROD-017 |

## M0 已实现的条款

| 条款 | 验证 |
|---|---|
| C-TIME-008 | M0 已实现：`NativeHost.monotonic_clock_never_decreases`（Native）；ESP32 使用 `esp_timer_get_time`，M2 补测 |
| C-LIFE-001 | M0 已实现：`KernelApi.create_in_caller_memory_starts_configuring` |
| C-LIFE-002 | M0 已实现 `CONFIGURING` 路径：`KernelApi.kernel_memory_is_reusable_after_destroy`；其他状态待 `MA-005` |
| C-LIMIT-004 | M0 已实现：ctest `core_does_not_reference_heap_allocator` 检查每个 Core 静态库不引用 `malloc`/`free`/`operator new`/`operator delete`，ctest `heap_check_detects_allocator` 证明该检查有效 |
| C-STATUS-001 | M0 已实现：`tests/unit/kernel_api_test.cpp` 覆盖 `ayni_create` 文档列出的每个失败码；`MA-005` 生命周期返回值表 |
| C-STATUS-002 | M0 已实现：`KernelApi.failed_create_leaves_memory_untouched` |
| C-VER-001 | M0 已实现：`KernelApi.create_rejects_other_abi_major`、`failed_create_leaves_memory_untouched` |
| C-VER-003 | M0 已实现：`KernelApi.create_copies_host_table_before_return`；M1 补旧尺寸用例 |
| C-VER-004 | M0 已实现：`KernelApi.struct_size_below_first_layout_is_invalid_argument`、`struct_size_from_newer_minor_is_unsupported_version` |
| C-HOST-001 | M0 已实现：`KernelApi.create_copies_host_table_before_return` |
| C-HOST-016 | M0 已实现：`KernelApi.create_rejects_missing_clock` |
