# Portable Core. Self-contained so that both the native project and the
# ESP-IDF runtime component can add it with add_subdirectory (ADR-0011).
# Modules get a target only once they have code; core/README.md lists the current module owners.

cmake_minimum_required(VERSION 3.22)

include("${CMAKE_CURRENT_LIST_DIR}/../cmake/AyniLayers.cmake")

ayni_add_core_module(foundation
    SOURCES
        foundation/crc32.cpp
        foundation/utf8.cpp)

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
        catalog/capability/call_contract_encoding.cpp
        catalog/capability/field_domain.cpp
        catalog/capability/field_values.cpp
        catalog/capability/argument_encoding.cpp
        catalog/capability/argument_mapping.cpp
        catalog/capability/value_validation.cpp
        catalog/capability/value_normalization.cpp
        catalog/capability/workspace.cpp
        catalog/capability/value_encoding.cpp
        catalog/capability/schema_encoding.cpp
        catalog/capability/authority_projection.cpp
        catalog/capability/business_encoding.cpp
        catalog/provider/provider_registry.cpp
        catalog/binding/binding_registry.cpp
        catalog/goal_type/goal_type_registry.cpp
        catalog/goal_type/work_profile.cpp
        catalog/goal_type/tool_policy.cpp
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
        authority/grant/grant_restore.cpp
        authority/grant/revocation_gate.cpp
        authority/principal/owner_binding_records.cpp
        authority/principal/principal_registry.cpp
        authority/scope/scope.cpp
        authority/scope/business_request.cpp
    DEPENDS catalog durability foundation)

ayni_add_core_module(resources
    SOURCES resources/admission/resource_admission.cpp
            resources/permit/permit_table.cpp
    DEPENDS catalog foundation)

ayni_add_core_module(knowledge
    SOURCES knowledge/observation/observation_table.cpp
            knowledge/observation/observation_input.cpp
            knowledge/memory/memory_types.cpp
            knowledge/memory/memory_table.cpp
            knowledge/memory/memory_records.cpp
            knowledge/context/context_table.cpp
            knowledge/context/context_records.cpp
    DEPENDS catalog durability foundation)

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
        work/subscription/subscription_fact.cpp
        work/call/call_table.cpp
        work/call/call_subscription.cpp
        work/call/tool_calls.cpp
        work/call_propagation.cpp
        work/execution/execution_fact.cpp
        work/goal/goal_authority.cpp
        work/goal/goal_authority_records.cpp
        work/goal/goal_records.cpp
        work/goal/goal_request.cpp
        work/goal/goal_table.cpp
        work/goal/pause_groups.cpp
        work/goal/pause_group_records.cpp
        work/goal/trigger_schedule.cpp
        work/goal/calendar.cpp
        work/goal/calendar_zone.cpp
        work/goal/calendar_table.cpp
        work/goal/calendar_records.cpp
        work/privacy/privacy_cleanup.cpp
        work/privacy/privacy_records.cpp
        work/provenance/provenance_records.cpp
        work/provenance/provenance_table.cpp
        work/propagation.cpp
        work/task/task_authority.cpp
        work/task/task_authority_records.cpp
        work/task/task_records.cpp
        work/task/task_table.cpp
    DEPENDS authority knowledge catalog durability foundation)

ayni_add_core_module(kernel
    SOURCES
        kernel/api/call_api.cpp
        kernel/api/directory_api.cpp
        kernel/api/calendar_api.cpp
        kernel/api/pause_group_api.cpp
        kernel/api/execution_api.cpp
        kernel/api/executor_completion_api.cpp
        kernel/api/kernel_api.cpp
        kernel/api/observation_api.cpp
        kernel/api/subscription_api.cpp
        kernel/api/provider_api.cpp
        kernel/api/provider_completion_api.cpp
        kernel/api/query_api.cpp
        kernel/api/resource_api.cpp
        kernel/api/memory_api.cpp
        kernel/api/scene_api.cpp
        kernel/api/work_api.cpp
        kernel/runtime/boot/boot_sequence.cpp
        kernel/service/call/call_admission.cpp
        kernel/service/call/call_operations.cpp
        kernel/service/call/call_directory.cpp
        kernel/service/call/call_completion.cpp
        kernel/service/call/call_dispatch.cpp
        kernel/service/call/call_drain.cpp
        kernel/service/call/call_reconciliation.cpp
        kernel/service/call/call_runtime.cpp
        kernel/service/call/call_subscription.cpp
        kernel/service/call/call_subscription_control.cpp
        kernel/service/call/call_tools.cpp
        kernel/service/call/call_wait.cpp
        kernel/service/command/command_approval.cpp
        kernel/service/command/command_memory.cpp
        kernel/service/command/command_canonical.cpp
        kernel/service/command/command_goal_identity.cpp
        kernel/service/command/command_reauthorization.cpp
        kernel/service/command/command_records.cpp
        kernel/service/command/command_scene.cpp
        kernel/service/command/command_pause_group.cpp
        kernel/service/command/command_table.cpp
        kernel/service/command/command_target.cpp
        kernel/service/command/command_task_limit.cpp
        kernel/service/command/pending_revoke.cpp
        kernel/service/command/work_authority_result.cpp
        kernel/runtime/commit/commit_log.cpp
        kernel/ports/completion_pool.cpp
        kernel/ports/executor_mailbox.cpp
        kernel/ports/provider_mailbox.cpp
        kernel/config/catalogs.cpp
        kernel/service/decision/decision.cpp
        kernel/service/execution/execution_control.cpp
        kernel/runtime/ingress/event_ingress.cpp
        kernel/runtime/ingress/observation_ingress.cpp
        kernel/runtime/ingress/observation_subscription_ingress.cpp
        kernel/runtime/ingress/event_queue.cpp
        kernel/service/intent/authority_intent.cpp
        kernel/service/intent/goal_approval.cpp
        kernel/service/model/goal_authority.cpp
        kernel/service/intent/goal_reauthorization.cpp
        kernel/service/intent/intent.cpp
        kernel/service/intent/memory_intent.cpp
        kernel/service/memory/memory_access.cpp
        kernel/service/memory/memory_permissions.cpp
        kernel/service/memory/memory_decision.cpp
        kernel/service/memory/privacy_service.cpp
        kernel/service/command/command_context.cpp
        kernel/service/memory/context_service.cpp
        kernel/service/intent/context_intent.cpp
        kernel/service/intent/pause_group_intent.cpp
        kernel/api/context_api.cpp
        kernel/service/intent/intent_rules.cpp
        kernel/service/model/task_authority.cpp
        kernel/service/model/tool_authority.cpp
        kernel/service/model/work_authority_cleanup.cpp
        kernel/service/model/work_provenance.cpp
        kernel/service/model/work_model.cpp
        kernel/runtime/kernel_intent.cpp
        kernel/runtime/kernel_facade.cpp
        kernel/runtime/kernel_layout.cpp
        kernel/runtime/kernel_lifecycle.cpp
        kernel/service/persistence/agent_store.cpp
        kernel/service/persistence/goal_store.cpp
        kernel/service/persistence/task_store.cpp
        kernel/service/persistence/call_store.cpp
        kernel/service/persistence/calendar_store.cpp
        kernel/service/persistence/pause_group_store.cpp
        kernel/service/persistence/grant_store.cpp
        kernel/service/persistence/command_store.cpp
        kernel/service/persistence/goal_approval_replay.cpp
        kernel/service/persistence/goal_reauthorization_replay.cpp
        kernel/service/persistence/provenance_store.cpp
        kernel/service/persistence/privacy_store.cpp
        kernel/service/persistence/context_store.cpp
        kernel/service/persistence/memory_store.cpp
        kernel/service/persistence/scene_store.cpp
        kernel/service/persistence/work_cleanup_replay.cpp
        kernel/service/persistence/work_journal.cpp
        kernel/service/persistence/work_replay.cpp
        kernel/service/persistence/work_snapshot.cpp
        kernel/runtime/storage/storage_budget.cpp
        kernel/runtime/storage/storage_maintenance.cpp
    DEPENDS work authority resources knowledge catalog durability foundation)

# What consumers link: the top module pulls in every module below it.
add_library(ayni_core INTERFACE)
target_link_libraries(ayni_core INTERFACE ayni_core_kernel)
