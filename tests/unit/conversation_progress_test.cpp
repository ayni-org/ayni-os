// C-SDK-009/011, C-HVAL-009: a shared Session crosses real file durability and Core
// publication once. Deterministic ordinary queue waits; no model service or fault injection.
#include "../../hosts/native/posix_files.h"
#include "temp_directory.h"
#include <ayni/hosts/sim.h>
#include <ayni/hosts/storage/file_value.h>
#include <ayni/runtime/portable.h>
#include <cstring>
#include <deque>
#include <gtest/gtest.h>
#include <map>
#include <new>
namespace {
using namespace ayni::runtime;
using namespace ayni::hosts::storage;
ayni_string_v1 text(const char *s) {
    return {s, static_cast<uint32_t>(std::strlen(s))};
}
// Accepted buffers stay borrowed until advance, mirroring the production asynchronous worker.
class Values {
  public:
    explicit Values(Files &files) : files_(files) {}
    FileValuePort port() { return {this, submit}; }
    void advance() {
        if (pending_.empty()) {
            return;
        }
        const auto request = pending_.front();
        pending_.pop_front();
        const auto result = serve_file_value(files_, request);
        request.complete(request.context, result);
    }
    bool empty() const { return pending_.empty(); }

  private:
    static ayni_submit_result_t submit(void *context, const FileValueRequest *request) {
        auto &self = *static_cast<Values *>(context);
        if (self.pending_.size() == portable::value_request_capacity) {
            return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
        }
        self.pending_.push_back(*request);
        return AYNI_SUBMIT_ACCEPTED;
    }
    Files &files_;
    std::deque<FileValueRequest> pending_;
};
struct ObservedBodies {
    enum class Pressure { none, after_success, observed };
    Pressure pressure = Pressure::none;
    const ayni_body_store_v1 *original = nullptr;
    std::map<uint64_t, uint32_t> reads, confirmed;
    ayni_body_store_v1 port{};
    void configure(const ayni_body_store_v1 *store) {
        original = store;
        port = *store;
        port.context = this;
        port.read = [](void *ctx, const ayni_body_ref_v1 *ref, uint8_t *out, uint32_t capacity) {
            auto &s = *static_cast<ObservedBodies *>(ctx);
            ++s.reads[ref->id];
            return s.original->read(s.original->context, ref, out, capacity);
        };
        port.confirm = [](void *ctx, const ayni_body_ref_v1 *ref) {
            auto &s = *static_cast<ObservedBodies *>(ctx);
            if (s.pressure == Pressure::after_success && !s.confirmed.empty()) {
                s.pressure = Pressure::observed;
                return AYNI_BUSY; // Ordinary queue backpressure after a successful prefix.
            }
            const auto status = s.original->confirm(s.original->context, ref);
            if (status == AYNI_OK) {
                ++s.confirmed[ref->id];
            }
            return status;
        };
        port.write = [](void *ctx, uint64_t scope, ayni_bytes_v1 bytes, ayni_body_ref_v1 *out) {
            auto &s = *static_cast<ObservedBodies *>(ctx);
            return s.original->write(s.original->context, scope, bytes, out);
        };
        port.stage = [](void *ctx, uint64_t scope, ayni_bytes_v1 bytes, ayni_body_ref_v1 *out) {
            auto &s = *static_cast<ObservedBodies *>(ctx);
            return s.original->stage(s.original->context, scope, bytes, out);
        };
        port.share = [](void *ctx, uint64_t scope, const ayni_body_ref_v1 *ref,
                        ayni_body_ref_v1 *out) {
            auto &s = *static_cast<ObservedBodies *>(ctx);
            return s.original->share(s.original->context, scope, ref, out);
        };
        port.release = [](void *ctx, const ayni_body_ref_v1 *ref) {
            auto &s = *static_cast<ObservedBodies *>(ctx);
            s.original->release(s.original->context, ref);
        };
        port.release_temporary = nullptr; // Session never owns Core's temporary request releases.
    }
};
class ConversationProgress : public testing::Test {
  protected:
    void SetUp() override {
        ASSERT_EQ(files.begin(directory.path().c_str()), 0);
        ASSERT_EQ(portable::Profile::memory_requirements(memory), AYNI_OK);
        arena = ::operator new(memory.bytes, std::align_val_t(memory.alignment), std::nothrow);
        ASSERT_NE(arena, nullptr);
        ASSERT_EQ(profile.create_in(arena, memory.bytes, host.table(), values.port(), persistence),
                  AYNI_OK);
        ASSERT_EQ(profile.start(), AYNI_OK);
        ASSERT_TRUE(until([&] { return profile.running(); }));
        for (const auto *id : {"fixture.tools", "fixture.summary", "software.sum"}) {
            ASSERT_TRUE(until([&] { return !profile.command_pending(); }));
            const ayni_grant_create_v1 grant{sizeof(grant),
                                             1,
                                             text(id),
                                             profile.instance(0),
                                             0,
                                             AYNI_GRANT_PERMANENT,
                                             0,
                                             0,
                                             {sizeof(ayni_scope_v1), 0, nullptr}};
            ASSERT_EQ(profile.submit(AYNI_INTENT_GRANT_CREATE, &grant).status, AYNI_OK);
        }
        ASSERT_TRUE(until([&] { return !profile.command_pending(); }));
        auto open = ayni::initialized<ayni_context_command_v1>();
        open.owner_instance_id = profile.instance(0);
        ASSERT_EQ(profile.submit(AYNI_INTENT_CONTEXT_OPEN, &open).status, AYNI_OK);
        ASSERT_TRUE(until([&] { return !profile.command_pending(); }));
        bind();
    }
    void bind() {
        observed.configure(profile.bodies());
        auto collection = static_cast<FileBodies *>(profile.bodies()->context)->collection();
        collection.store = &observed.port;
        ASSERT_EQ(session.configure({profile.kernel(),
                                     profile.session(),
                                     profile.instance(0),
                                     text("portable.chat.a"),
                                     profile.value_workspace(),
                                     values.port(),
                                     collection,
                                     {this, command},
                                     conversation::UnfinishedTurn::discard,
                                     persistence}),
                  AYNI_OK);
        bound = true;
        ASSERT_TRUE(until([&] { return session.ready(); }));
    }
    void TearDown() override {
        if (profile.kernel()) {
            if (bound) {
                EXPECT_TRUE(until([&] { return session.quiesce() == AYNI_OK; }));
                bound = false;
            }
            EXPECT_EQ(profile.begin_shutdown(), AYNI_OK);
            EXPECT_TRUE(until([&] { return profile.stopped() && values.empty(); }));
            EXPECT_EQ(profile.destroy(), AYNI_OK);
        }
        if (arena && !profile.kernel()) {
            ::operator delete(arena, std::align_val_t(memory.alignment));
        }
    }
    template <class Done> bool until(Done done) {
        constexpr uint32_t tick_limit = 20000, worker_period = 3;
        for (uint32_t tick = 0; tick < tick_limit; ++tick) {
            if (done()) {
                return true;
            }
            const auto status = profile.tick();
            if (status != AYNI_OK && status != AYNI_BUSY && status != AYNI_NOT_READY) {
                ADD_FAILURE() << "profile=" << status;
                return false;
            }
            if (bound) {
                const auto polled = session.poll();
                if (polled != AYNI_OK && polled != AYNI_BUSY && polled != AYNI_NOT_READY) {
                    ADD_FAILURE() << "session=" << polled;
                    return false;
                }
            }
            if (tick % worker_period == 0) {
                values.advance();
            }
            host.complete_storage();
            host.advance_us(1000);
        }
        ADD_FAILURE() << "normal progress deadline";
        return false;
    }
    ayni_context_info_v1 context() {
        auto anchor = ayni::initialized<ayni_context_info_v1>();
        EXPECT_EQ(
            ayni_get_context(profile.kernel(), profile.session(), profile.instance(0), &anchor),
            AYNI_OK);
        return anchor;
    }
    static ayni::Submission command(void *ctx, ayni_intent_kind_t kind, const void *body,
                                    uint32_t revision) {
        auto &self = *static_cast<ConversationProgress *>(ctx);
        const auto accepted = self.profile.submit(kind, body, revision);
        if (accepted.status == AYNI_OK) {
            ++self.commands[kind];
        }
        return accepted;
    }
    void wait_completed_task() {
        ASSERT_TRUE(until([&] {
            auto goal = ayni::initialized<ayni_goal_info_v1>();
            auto task = ayni::initialized<ayni_task_info_v1>();
            return session.latest_goal() &&
                   ayni_get_goal(profile.kernel(), session.latest_goal(), &goal) == AYNI_OK &&
                   goal.last_task_id &&
                   ayni_get_task(profile.kernel(), goal.last_task_id, &task) == AYNI_OK &&
                   task.lifecycle == AYNI_TASK_COMPLETED;
        }));
        observed.reads.clear();
        observed.confirmed.clear();
    }
    ayni::strategies::Persistence persistence = ayni::strategies::Persistence::background;
    ayni::test::TempDirectory directory;
    ayni::hosts::native::PosixFiles files;
    Values values{files};
    ayni::hosts::sim::Host host;
    portable::Profile profile;
    portable::Memory memory;
    void *arena = nullptr;
    conversation::Session session;
    ObservedBodies observed;
    bool bound = false;
    std::map<ayni_intent_kind_t, uint32_t> commands;
};
TEST_F(ConversationProgress, background_publication_reuses_dependencies_and_waits_for_exact_ack) {
    ASSERT_EQ(session.begin(text("sum")), AYNI_OK);
    wait_completed_task();
    observed.pressure = ObservedBodies::Pressure::after_success;
    ASSERT_FALSE(session.ready());
    ASSERT_TRUE(until([&] { return context().pending_action == AYNI_CONTEXT_PUBLISH; }));
    ASSERT_FALSE(session.ready());
    conversation::TurnResult result;
    EXPECT_FALSE(session.take_result(result));
    ASSERT_TRUE(until([&] { return session.take_result(result); }));
    ASSERT_TRUE(result.published);
    ASSERT_NE(session.record(), nullptr);
    EXPECT_EQ(observed.reads[session.record()->history.id], 1U);
    ASSERT_FALSE(observed.confirmed.empty());
    EXPECT_EQ(observed.pressure, ObservedBodies::Pressure::observed);
    for (const auto &[id, count] : observed.confirmed) {
        EXPECT_EQ(count, 1U) << "reference=" << id;
    }
    EXPECT_EQ(commands[AYNI_INTENT_CONTEXT_PUBLISH], 1U);
    EXPECT_EQ(commands[AYNI_INTENT_CONTEXT_ACK], 1U);
    const auto anchor = context();
    auto info = ayni::initialized<ayni_kernel_info_v1>();
    ASSERT_EQ(ayni_get_kernel_info(profile.kernel(), &info), AYNI_OK);
    EXPECT_LE(anchor.commit_seq, info.durable_seq);
    EXPECT_EQ(anchor.pending_action, AYNI_CONTEXT_IDLE);
}
TEST_F(ConversationProgress, cancel_during_publication_preserves_the_saved_answer_and_forget) {
    ASSERT_EQ(session.begin(text("sum")), AYNI_OK);
    ASSERT_TRUE(until([&] { return context().pending_action == AYNI_CONTEXT_PUBLISH; }));
    ASSERT_EQ(session.cancel(), AYNI_OK);
    conversation::TurnResult result;
    ASSERT_TRUE(until([&] { return session.take_result(result); }));
    ASSERT_TRUE(result.published);
    const auto old = session.record()->history;
    ASSERT_EQ(session.forget(AYNI_CONTEXT_ERASE_ALL), AYNI_OK);
    ASSERT_TRUE(until([&] { return session.ready(); }));
    EXPECT_EQ(session.record()->round_count, 0U);
    std::string bytes(old.size, '\0');
    auto status = AYNI_NOT_READY;
    ASSERT_TRUE(until([&] {
        status = profile.bodies()->read(profile.bodies()->context, &old,
                                        reinterpret_cast<uint8_t *>(bytes.data()), old.size);
        return status != AYNI_NOT_READY;
    }));
    EXPECT_EQ(status, AYNI_NOT_FOUND);
    ASSERT_EQ(session.begin(text("sum")), AYNI_OK);
    ASSERT_TRUE(until([&] { return session.take_result(result); }));
    EXPECT_TRUE(result.published);
}
// C-SDK-009: a clean stop/reopen uses the saved complete history, with no replayed model call.
TEST_F(ConversationProgress, normal_close_reopen_retains_answer_without_another_model) {
    ASSERT_EQ(session.begin(text("sum")), AYNI_OK);
    conversation::TurnResult result;
    ASSERT_TRUE(until([&] { return session.take_result(result); }));
    ASSERT_TRUE(result.published);
    const auto last = session.record()->last_round;
    ASSERT_TRUE(until([&] { return session.quiesce() == AYNI_OK; }));
    bound = false;
    ASSERT_EQ(profile.begin_shutdown(), AYNI_OK);
    ASSERT_TRUE(until([&] { return profile.stopped() && values.empty(); }));
    ASSERT_EQ(profile.destroy(), AYNI_OK);
    // Both callback owners are drained; restart the placed Session on its fresh Core handle.
    session.~Session();
    new (&session) conversation::Session();
    files.end();
    ASSERT_EQ(files.begin(directory.path().c_str()), 0);
    ASSERT_EQ(profile.create_in(arena, memory.bytes, host.table(), values.port(), persistence),
              AYNI_OK);
    ASSERT_EQ(profile.start(), AYNI_OK);
    ASSERT_TRUE(until([&] { return profile.running(); }));
    bind();
    ASSERT_TRUE(session.ready());
    EXPECT_EQ(session.record()->last_round, last);
    EXPECT_EQ(profile.model_submissions(), 0U);
    std::string answer;
    ASSERT_TRUE(until([&] {
        return ayni::strategies::Conversation::latest_answer(*profile.bodies(), *session.record(),
                                                             answer) == AYNI_OK;
    }));
    EXPECT_FALSE(answer.empty());
}
class DurableConversationProgress : public ConversationProgress {
  protected:
    DurableConversationProgress() { persistence = ayni::strategies::Persistence::durable; }
};
TEST_F(DurableConversationProgress, durable_default_completes_the_same_shared_publication) {
    ASSERT_EQ(session.begin(text("sum")), AYNI_OK);
    conversation::TurnResult result;
    ASSERT_TRUE(until([&] { return session.take_result(result); }));
    EXPECT_TRUE(result.published);
    EXPECT_EQ(session.record()->last_round, 1U);
}
} // namespace
