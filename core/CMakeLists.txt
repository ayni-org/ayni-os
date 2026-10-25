# Portable Core. Self-contained so that both the native project and the
# ESP-IDF runtime component can add it with add_subdirectory (ADR-0011).
# Modules get a target only once they have code; core/README.md lists the current module owners.

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
        catalog/observation/source_registry.cpp
        catalog/execution/execution_registry.cpp
        catalog/capability/capability_registry.cpp
        catalog/capability/call_contract.cpp
        catalog/capability/field_domain.cpp
        catalog/capability/field_values.cpp
        catalog/provider/provider_registry.cpp
        catalog/binding/binding_registry.cpp
        catalog/goal_type/goal_type_registry.cpp
        catalog/goal_type/work_profile.cpp
        catalog/device/device_catalog.cpp
        catalog/resource/resource_registry.cpp
        catalog/strategy/strategy_registry.cpp
    DEPENDS foundation)

ayni_add_core_module(authority
    SOURCES
        authority/scene/scene_definition.cpp
        authority/scene/scene_table.cpp
        authority/scene/scene_records.cpp
        authority/grant/grant_records.cpp
        authority/grant/grant_table.cpp
        authority/principal/owner_binding_records.cpp
        authority/principal/principal_registry.cpp
        authority/scope/scope.cpp
    DEPENDS catalog durability foundation)

ayni_add_core_module(resources
    SOURCES resources/admission/resource_admission.cpp
            resources/permit/permit_table.cpp
    DEPENDS catalog foundation)

ayni_add_core_module(knowledge
    SOURCES knowledge/observation/observation_table.cpp
    DEPENDS catalog foundation)

ayni_add_core_module(work
    SOURCES
        work/action/action_record.cpp
        work/action/action_records.cpp
        work/agent/agent_records.cpp
        work/agent/agent_table.cpp
        work/authority/authority_cleanup.cpp
        work/authority/authority_cleanup_records.cpp
        work/authority/authority_facts.cpp
        work/call/call_input.cpp
        work/call/call_query_validation.cpp
        work/call/call_reconciliation.cpp
        work/call/call_records.cpp
        work/call/call_restore.cpp
        work/call/call_table.cpp
        work/execution/execution_fact.cpp
        work/goal/goal_authority.cpp
        work/goal/goal_authority_records.cpp
        work/goal/goal_records.cpp
        work/goal/goal_request.cpp
        work/goal/goal_table.cpp
        work/goal/trigger_schedule.cpp
        work/propagation.cpp
        work/task/task_authority.cpp
        work/task/task_authority_records.cpp
        work/task/task_records.cpp
        work/task/task_table.cpp
    DEPENDS authority catalog durability foundation)

ayni_add_core_module(kernel
    SOURCES
        kernel/api/call_api.cpp
        kernel/api/execution_api.cpp
        kernel/api/kernel_api.cpp
        kernel/api/observation_api.cpp
        kernel/api/provider_api.cpp
        kernel/api/provider_completion_api.cpp
        kernel/api/resource_api.cpp
        kernel/api/scene_api.cpp
        kernel/api/work_api.cpp
        kernel/boot/boot_sequence.cpp
        kernel/call/call_admission.cpp
        kernel/call/call_completion.cpp
        kernel/call/call_dispatch.cpp
        kernel/call/call_drain.cpp
        kernel/call/call_reconciliation.cpp
        kernel/call/call_runtime.cpp
        kernel/call/call_wait.cpp
        kernel/command/command_approval.cpp
        kernel/command/command_canonical.cpp
        kernel/command/command_goal_identity.cpp
        kernel/command/command_reauthorization.cpp
        kernel/command/command_records.cpp
        kernel/command/command_scene.cpp
        kernel/command/command_table.cpp
        kernel/command/command_target.cpp
        kernel/command/command_task_limit.cpp
        kernel/command/pending_revoke.cpp
        kernel/command/work_authority_result.cpp
        kernel/commit/commit_log.cpp
        kernel/completion/completion_pool.cpp
        kernel/completion/provider_mailbox.cpp
        kernel/config/catalogs.cpp
        kernel/decision/decision.cpp
        kernel/execution/execution_control.cpp
        kernel/ingress/event_ingress.cpp
        kernel/ingress/event_queue.cpp
        kernel/intent/authority_intent.cpp
        kernel/intent/goal_approval.cpp
        kernel/intent/goal_authority.cpp
        kernel/intent/goal_reauthorization.cpp
        kernel/intent/intent.cpp
        kernel/intent/intent_rules.cpp
        kernel/intent/task_authority.cpp
        kernel/intent/work_authority_cleanup.cpp
        kernel/intent/work_model.cpp
        kernel/kernel_intent.cpp
        kernel/kernel_layout.cpp
        kernel/kernel_lifecycle.cpp
        kernel/persistence/call_store.cpp
        kernel/persistence/goal_approval_replay.cpp
        kernel/persistence/goal_reauthorization_replay.cpp
        kernel/persistence/scene_store.cpp
        kernel/persistence/work_cleanup_replay.cpp
        kernel/persistence/work_journal.cpp
        kernel/persistence/work_replay.cpp
        kernel/persistence/work_snapshot.cpp
        kernel/storage/storage_budget.cpp
        kernel/storage/storage_maintenance.cpp
    DEPENDS work authority resources knowledge catalog durability foundation)

# What consumers link: the top module pulls in every module below it.
add_library(ayni_core INTERFACE)
target_link_libraries(ayni_core INTERFACE ayni_core_kernel)
