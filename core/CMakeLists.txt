# Portable Core. Self-contained so that both the native project and the
# ESP-IDF runtime component can add it with add_subdirectory (ADR-0011).
# Modules get a target only once they have code; core/README.md lists all nine.

cmake_minimum_required(VERSION 3.22)

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/AyniLayers.cmake")

ayni_add_core_module(foundation
    SOURCES
        foundation/crc32.cpp)

ayni_add_core_module(durability
    SOURCES
        durability/batch/batch_codec.cpp
        durability/batch/prefix_scan.cpp
        durability/journal/journal_ring.cpp
        durability/snapshot/snapshot_codec.cpp
    DEPENDS foundation)

ayni_add_core_module(catalog
    SOURCES
        catalog/catalog_id.cpp
        catalog/capability/capability_registry.cpp
        catalog/capability/field_domain.cpp
        catalog/capability/field_values.cpp
        catalog/goal_type/goal_type_registry.cpp
        catalog/strategy/strategy_registry.cpp
    DEPENDS foundation)

ayni_add_core_module(authority
    SOURCES
        authority/grant/grant_records.cpp
        authority/grant/grant_table.cpp
        authority/principal/owner_binding_records.cpp
        authority/principal/principal_registry.cpp
        authority/scope/scope.cpp
    DEPENDS catalog durability foundation)

ayni_add_core_module(work
    SOURCES
        work/authority/authority_facts.cpp
        work/agent/agent_records.cpp
        work/agent/agent_table.cpp
        work/goal/goal_authority.cpp
        work/goal/goal_authority_records.cpp
        work/goal/goal_records.cpp
        work/goal/goal_table.cpp
        work/goal/goal_request.cpp
        work/goal/trigger_schedule.cpp
        work/propagation.cpp
        work/task/task_records.cpp
        work/task/task_authority.cpp
        work/task/task_authority_records.cpp
        work/task/task_table.cpp
    DEPENDS authority catalog durability foundation)

ayni_add_core_module(kernel
    SOURCES
        kernel/boot/boot_sequence.cpp
        kernel/command/command_canonical.cpp
        kernel/command/command_approval.cpp
        kernel/command/command_records.cpp
        kernel/command/command_table.cpp
        kernel/command/pending_revoke.cpp
        kernel/commit/commit_log.cpp
        kernel/completion/completion_pool.cpp
        kernel/ingress/event_ingress.cpp
        kernel/ingress/event_queue.cpp
        kernel/intent/authority_intent.cpp
    kernel/intent/goal_approval.cpp
        kernel/intent_receipt.cpp
        kernel/intent/decision.cpp
        kernel/intent/intent.cpp
        kernel/intent/intent_rules.cpp
        kernel/intent/goal_authority.cpp
        kernel/intent/work_model.cpp
        kernel/kernel_api.cpp
        kernel/kernel_layout.cpp
        kernel/kernel_lifecycle.cpp
        kernel/storage/storage_budget.cpp
        kernel/storage/storage_maintenance.cpp
        kernel/work_api.cpp
        kernel/work_journal.cpp
        kernel/work_replay.cpp
        kernel/goal_approval_replay.cpp
        kernel/work_snapshot.cpp
    DEPENDS work authority catalog durability foundation)

# What consumers link: the top module pulls in every module below it.
add_library(ayni_core INTERFACE)
target_link_libraries(ayni_core INTERFACE ayni_core_kernel)
