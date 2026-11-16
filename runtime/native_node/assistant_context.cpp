// Shared Session owns conversation state and all publication/erasure barriers.
// Native maps the common ports and turns durable observations into UI notices.
#include "assistant_state.h"
#include <chrono>
namespace ayni::runtime {
ayni_status_t Assistant::Impl::context_info(ayni_context_info_v1 &context) const {
    context = initialized<ayni_context_info_v1>();
    return ayni_get_context(node.kernel(), session, assistant(), &context);
}
ayni_status_t Assistant::Impl::initialize_context() {
    auto anchor = initialized<ayni_context_info_v1>();
    auto status = context_info(anchor);
    if (status == AYNI_NOT_FOUND) {
        auto body = initialized<ayni_context_command_v1>();
        body.owner_instance_id = assistant();
        const auto accepted = submit(AYNI_INTENT_CONTEXT_OPEN, &body);
        if (accepted.status != AYNI_OK) {
            return accepted.status;
        }
    } else if (status != AYNI_OK) {
        return status;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    do {
        status = context_info(anchor);
        auto info = initialized<ayni_kernel_info_v1>();
        if (status == AYNI_OK) {
            status = ayni_get_kernel_info(node.kernel(), &info);
        }
        if (status != AYNI_OK || info.durable_seq >= anchor.commit_seq) {
            break;
        }
        status = advance_kernel();
        if (status != AYNI_OK || std::chrono::steady_clock::now() >= deadline) {
            return status != AYNI_OK ? status : AYNI_BUSY;
        }
        node.wait_for_wake(std::chrono::milliseconds(1));
    } while (true);
    if (status != AYNI_OK) {
        return status;
    }
    conversation::Config config;
    config.kernel = node.kernel();
    config.session = session;
    config.owner_instance = assistant();
    config.goal_type = runtime_text("chat");
    config.workspace = conversation_workspace.get();
    config.files = store.files();
    config.bodies = bodies.collection();
    config.unfinished = conversation::UnfinishedTurn::retain_and_append;
    config.commands = {
        this, [](void *context, ayni_intent_kind_t kind, const void *body, uint32_t revision) {
            const auto reply = static_cast<Impl *>(context)->submit(kind, body, revision);
            Submission result;
            result.status = reply.status;
            result.result.object.id = reply.id;
            result.result.commit_seq = reply.commit_seq;
            result.result.phase = reply.phase;
            result.error.reason = reply.reason;
            return result;
        }};
    return conversation.configure(config);
}
ayni_status_t Assistant::Impl::collect(std::vector<Notice> &notices) {
    const auto status = conversation.poll();
    if (status != AYNI_OK && status != AYNI_BUSY && !conversation.held() &&
        !(status == AYNI_NOT_READY && conversation.record())) {
        storage_status = status;
        return status;
    }
    if (const auto *result = conversation.pending_result()) {
        if (result->published) {
            const auto *record = conversation.record();
            if (!record) {
                return AYNI_NOT_READY;
            }
            std::string answer;
            const auto read =
                strategies::Conversation::latest_answer(*bodies.store(), *record, answer);
            if (read != AYNI_OK) {
                return read == AYNI_NOT_READY ? AYNI_OK : read;
            }
            notices.push_back({NoticeKind::answer, std::move(answer)});
        } else {
            notices.push_back({NoticeKind::turn_aborted, {}});
        }
        conversation::TurnResult consumed;
        conversation.take_result(consumed);
    }
    if (const auto *record = conversation.record()) {
        if (notified_generation && notified_generation != record->generation) {
            notices.push_back({NoticeKind::context_erased, {}});
        }
        notified_generation = record->generation;
    }
    return AYNI_OK;
}
} // namespace ayni::runtime
