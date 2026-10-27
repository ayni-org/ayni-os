// Owner CLI workflows use public memory and Grant contracts; no Core representation is decoded
// here.
#include "assistant_state.h"
namespace ayni::runtime {
namespace {
Reply refused(ayni_status_t status) {
    Reply reply;
    reply.status = status;
    return reply;
}
} // namespace
Reply Assistant::Impl::memory(const Command &command) {
    Reply reply;
    const auto op = command.operation;
    const bool owner_profile = op == Operation::profile_list || op == Operation::profile_save;
    if (op == Operation::memory_list || op == Operation::profile_list) {
        const ayni_memory_search_v1 query{sizeof(query),
                                          owner_profile ? AYNI_MEMORY_PROFILE : AYNI_MEMORY_PRIVATE,
                                          owner_profile ? demo_owner : instance(command.agent),
                                          0,
                                          runtime_text(command.text),
                                          {nullptr, 0}};
        std::array<ayni_memory_info_v1, AYNI_MEMORY_CAPACITY_MAX> rows{};
        uint32_t count = 0;
        reply.status =
            ayni_search_memory(node.kernel(), session, &query, rows.data(), rows.size(), &count);
        if (reply.status == AYNI_OK) {
            for (uint32_t at = 0; at < count; ++at) {
                reply.memories.push_back({rows[at], {}});
            }
        }
        return reply;
    }
    ayni_memory_info_v1 info{};
    info.struct_size = sizeof(info);
    if (op != Operation::memory_save && op != Operation::profile_save) {
        uint32_t size = 0;
        reply.status =
            ayni_get_memory(node.kernel(), session, command.id, &info, nullptr, 0, &size);
        if (reply.status != AYNI_OK) {
            return reply;
        }
    }
    if (op == Operation::memory_show) {
        std::array<char, AYNI_MEMORY_BODY_BYTES_MAX> body{};
        uint32_t size = 0;
        reply.status = ayni_get_memory(node.kernel(), session, command.id, &info, body.data(),
                                       body.size(), &size);
        if (reply.status == AYNI_OK) {
            reply.memories.push_back({info, std::string(body.data(), size)});
        }
        return reply;
    }
    if (op == Operation::memory_share || op == Operation::memory_export) {
        std::array<ayni_scope_field_v1, AYNI_MEMORY_SCOPE_FIELD_COUNT> fields{};
        const auto destination = this->profile.destination();
        const bool exporting = op == Operation::memory_export;
        if (exporting && destination.size == 0) {
            return refused(AYNI_UNSUPPORTED);
        }
        const auto status =
            exporting
                ? ayni_memory_export_scope(command.id, command.purpose, &destination, fields.data(),
                                           fields.size())
                : ayni_memory_read_scope(command.id, command.purpose, fields.data(), fields.size());
        if (status != AYNI_OK) {
            return refused(status);
        }
        const ayni_grant_create_v1 grant{
            sizeof(grant),
            1,
            runtime_text(exporting ? "memory.egress" : "memory.access"),
            instance(command.agent),
            0,
            AYNI_GRANT_PERMANENT,
            0,
            0,
            {sizeof(ayni_scope_v1), fields.size(), fields.data()}};
        return submit(AYNI_INTENT_GRANT_CREATE, &grant);
    }
    if (op == Operation::memory_confirm || op == Operation::memory_delete) {
        const ayni_memory_target_v1 target{sizeof(target), 0, command.id};
        return submit(op == Operation::memory_confirm ? AYNI_INTENT_MEMORY_CONFIRM
                                                      : AYNI_INTENT_MEMORY_DELETE,
                      &target, info.revision);
    }
    if (op == Operation::memory_save || op == Operation::memory_edit ||
        op == Operation::profile_save) {
        ayni_memory_save_v1 value{};
        value.struct_size = sizeof(value);
        const bool edit = op == Operation::memory_edit;
        value.name_space = edit            ? info.name_space
                           : owner_profile ? AYNI_MEMORY_PROFILE
                                           : AYNI_MEMORY_PRIVATE;
        value.memory_id = edit ? command.id : 0;
        value.namespace_id = edit            ? info.namespace_id
                             : owner_profile ? demo_owner
                                             : instance(command.agent);
        value.profile_field = edit ? info.profile_field : command.profile_field;
        value.body = runtime_text(command.text);
        return submit(AYNI_INTENT_MEMORY_SAVE, &value, edit ? info.revision : 0);
    }
    return refused(AYNI_UNSUPPORTED);
}
} // namespace ayni::runtime
