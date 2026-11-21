// C-SDK-009/011, C-HVAL-009: a shared Session crosses real file durability and Core
// publication once. Deterministic ordinary queue waits; no model service or fault injection.
#include "../../hosts/native/posix_files.h"
#include "temp_directory.h"
#include <array>
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
    uint32_t collect_busy_remaining = 0;
    uint32_t collect_busy_observed = 0;
    const ayni_body_store_v1 *original = nullptr;
    BodyCollection collection;
    std::map<std::string, uint32_t> staged;
    bool queue_unrelated = false;
    ayni_body_ref_v1 unrelated{};
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
            ++s.staged[std::string(reinterpret_cast<const char *>(bytes.data), bytes.size)];
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
        observed.collection = collection;
        collection.store = &observed.port;
        collection.context = &observed;
        collection.collect = [](void *ctx, uint64_t scope,
                                const std::vector<ayni_body_ref_v1> &keep) {
            auto &s = *static_cast<ObservedBodies *>(ctx);
            s.collection.collect(s.collection.context, scope, keep);
        };
        collection.ready = [](void *ctx, uint64_t task) {
            auto &s = *static_cast<ObservedBodies *>(ctx);
            return s.collection.ready(s.collection.context, task);
        };
        collection.progress = [](void *ctx, uint64_t task) {
            auto &s = *static_cast<ObservedBodies *>(ctx);
            return s.collection.progress(s.collection.context, task);
        };
        collection.status = [](void *ctx) {
            auto &s = *static_cast<ObservedBodies *>(ctx);
            return s.collection.status(s.collection.context);
        };
        collection.collected = [](void *ctx, uint64_t scope) {
            auto &s = *static_cast<ObservedBodies *>(ctx);
            if (s.collect_busy_remaining > 0) {
                --s.collect_busy_remaining;
                ++s.collect_busy_observed;
                return AYNI_BUSY;
            }
            const auto status = s.collection.collected(s.collection.context, scope);
            if (status == AYNI_OK && s.queue_unrelated && !s.unrelated.id) {
                constexpr uint64_t other_scope = UINT64_C(0xfeed);
                const std::string bytes(128U * 1024U, 'x');
                EXPECT_EQ(s.original->stage(s.original->context, other_scope,
                                            {reinterpret_cast<const uint8_t *>(bytes.data()),
                                             uint32_t(bytes.size())},
                                            &s.unrelated),
                          AYNI_OK);
                EXPECT_EQ(s.original->confirm(s.original->context, &s.unrelated), AYNI_NOT_READY);
            }
            return status;
        };
        ASSERT_EQ(session.configure({profile.kernel(),
                                     profile.session(),
                                     profile.instance(0),
                                     text("portable.chat.a"),
                                     profile.value_workspace(),
                                     values.port(),
                                     collection,
                                     {this, command},
                                     conversation::UnfinishedTurn::discard,
                                     persistence,
                                     {this, on_publication_stage}}),
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
    struct StageEvent {
        uint64_t goal = 0, task = 0, at_us = 0;
        conversation::PublicationStage stage;
    };
    static void on_publication_stage(void *ctx, uint64_t goal, uint64_t task,
                                     conversation::PublicationStage stage) {
        auto &self = *static_cast<ConversationProgress *>(ctx);
        self.stage_events.push_back({goal, task, self.host.now_us(), stage});
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
    void verify_publication_timeline(
        const std::array<conversation::PublicationStage, 5> &expected_order) {
        // Seed identical initial history so the measured turn replaces an existing graph.
        ASSERT_EQ(session.begin(text("seed initial turn")), AYNI_OK);
        conversation::TurnResult seed;
        ASSERT_TRUE(until([&] { return session.take_result(seed); }));
        ASSERT_TRUE(seed.published);
        ASSERT_EQ(session.record()->round_count, 1U);

        stage_events.clear();
        observed.confirmed.clear();
        observed.pressure = ObservedBodies::Pressure::after_success;
        observed.collect_busy_remaining = 3;
        observed.collect_busy_observed = 0;

        ASSERT_EQ(session.begin(text("fixed normal text turn")), AYNI_OK);
        uint64_t model_posted_us = 0;
        ASSERT_TRUE(until([&] {
            if (!session.latest_goal() || session.latest_goal() == seed.goal) {
                return false;
            }
            uint64_t calls[16]{};
            uint32_t count = 0;
            if (ayni_list_objects(profile.kernel(), AYNI_OBJECT_CALL, calls, 16, &count) !=
                AYNI_OK) {
                return false;
            }
            auto goal = ayni::initialized<ayni_goal_info_v1>();
            if (ayni_get_goal(profile.kernel(), session.latest_goal(), &goal) != AYNI_OK ||
                !goal.last_task_id) {
                return false;
            }
            for (uint32_t at = 0; at < count; ++at) {
                auto call = ayni::initialized<ayni_call_info_v1>();
                if (ayni_get_call(profile.kernel(), calls[at], &call) == AYNI_OK &&
                    call.task_id == goal.last_task_id && call.outcome == AYNI_CALL_COMPLETED) {
                    model_posted_us = host.now_us();
                    return true;
                }
            }
            return false;
        }));
        EXPECT_TRUE(stage_events.empty());
        conversation::TurnResult result;
        ASSERT_TRUE(until([&] { return session.take_result(result); }));
        ASSERT_TRUE(result.published);
        ASSERT_NE(result.goal, 0U);
        ASSERT_NE(result.task, 0U);
        std::string answer;
        ASSERT_TRUE(until([&] {
            return ayni::strategies::Conversation::latest_answer(
                       *profile.bodies(), *session.record(), answer) == AYNI_OK;
        }));
        const uint64_t answer_saved_us = host.now_us();
        EXPECT_FALSE(answer.empty());
        EXPECT_EQ(observed.pressure, ObservedBodies::Pressure::observed);
        EXPECT_EQ(observed.collect_busy_observed, 3U);

        ASSERT_EQ(stage_events.size(), expected_order.size());
        uint64_t prev = model_posted_us, sum_us = 0;
        for (size_t at = 0; at < expected_order.size(); ++at) {
            EXPECT_EQ(stage_events[at].goal, result.goal);
            EXPECT_EQ(stage_events[at].task, result.task);
            EXPECT_EQ(stage_events[at].stage, expected_order[at]);
            EXPECT_GE(stage_events[at].at_us, prev);
            sum_us += stage_events[at].at_us - prev;
            prev = stage_events[at].at_us;
        }
        EXPECT_GE(answer_saved_us, prev);
        sum_us += answer_saved_us - prev;
        EXPECT_EQ(sum_us, answer_saved_us - model_posted_us);
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
    std::vector<StageEvent> stage_events;
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
// C-HVAL-009/C-SDK-009: default durable admission stages once through ordinary worker delay.
TEST_F(DurableConversationProgress, waiting_for_input_durability_does_not_restage_the_input) {
    ASSERT_EQ(session.begin(text("sum")), AYNI_OK);
    for (uint32_t poll = 0; poll < 32; ++poll) {
        const auto status = session.poll();
        ASSERT_TRUE(status == AYNI_OK || status == AYNI_BUSY);
    }
    EXPECT_EQ(session.latest_goal(), 0U);
    EXPECT_EQ(observed.staged["sum"], 1U);
    conversation::TurnResult result;
    ASSERT_TRUE(until([&] { return session.take_result(result); }));
    EXPECT_TRUE(result.published);
    EXPECT_EQ(observed.staged["sum"], 1U);
}
// C-SDK-009/011: this graph's cleanup and exact ACK complete despite another scope's write.
TEST_F(ConversationProgress, publication_does_not_wait_for_unrelated_body_persistence) {
    ASSERT_EQ(session.begin(text("sum")), AYNI_OK);
    observed.queue_unrelated = true;
    conversation::TurnResult result;
    ASSERT_TRUE(until([&] { return session.take_result(result); }));
    EXPECT_TRUE(result.published);
    EXPECT_NE(observed.unrelated.id, 0U);
    EXPECT_FALSE(static_cast<FileBodies *>(profile.bodies()->context)->idle());
    EXPECT_EQ(context().pending_action, AYNI_CONTEXT_IDLE);
}
// C-SDK-009: an asynchronous collection adapter cannot silently omit its cleanup proof.
TEST_F(ConversationProgress, asynchronous_collection_requires_scoped_completion) {
    conversation::Session unconfigured;
    auto collection = static_cast<FileBodies *>(profile.bodies()->context)->collection();
    collection.collected = nullptr;
    conversation::Config config;
    config.kernel = profile.kernel();
    config.session = profile.session();
    config.owner_instance = profile.instance(0);
    config.goal_type = text("portable.chat.a");
    config.workspace = profile.value_workspace();
    config.files = values.port();
    config.bodies = collection;
    config.commands = {this, command};
    EXPECT_EQ(unconfigured.configure(config), AYNI_INVALID_ARGUMENT);
}
// C-SDK-009: background publication observes each boundary once in actual async order with retries.
TEST_F(ConversationProgress,
       background_publication_emits_ordered_unique_milestones_with_additive_segments) {
    using Stage = conversation::PublicationStage;
    verify_publication_timeline({Stage::checkpoint_ready, Stage::publish_durable,
                                 Stage::record_saved, Stage::collected, Stage::context_ready});
}
// C-SDK-009: durable publication preserves the direct path's publish_durable -> checkpoint order.
TEST_F(DurableConversationProgress,
       durable_publication_preserves_direct_order_and_additive_segments) {
    using Stage = conversation::PublicationStage;
    verify_publication_timeline({Stage::publish_durable, Stage::checkpoint_ready,
                                 Stage::record_saved, Stage::collected, Stage::context_ready});
}

class ScopedBodyCleanup : public testing::Test {
  protected:
    void SetUp() override {
        ASSERT_EQ(files.begin(directory.path().c_str()), 0);
        ASSERT_EQ(bodies.configure(values.port(), 2U * 1024U * 1024U), AYNI_OK);
        ASSERT_EQ(reopened.configure(values.port(), 2U * 1024U * 1024U), AYNI_OK);
    }
    void TearDown() override {
        EXPECT_TRUE(until([&] { return bodies.idle() && reopened.idle(); }));
    }
    template <class Done> bool until(Done done) {
        constexpr uint32_t limit = 10000;
        for (uint32_t at = 0; at < limit && !done(); ++at) {
            FileBodies::progress(&bodies, 0);
            FileBodies::progress(&reopened, 0);
            values.advance();
        }
        return done();
    }
    ayni_body_ref_v1 stage(uint64_t scope, const char *text) {
        const auto *store = bodies.store();
        ayni_body_ref_v1 ref{};
        EXPECT_EQ(store->stage(
                      store->context, scope,
                      {reinterpret_cast<const uint8_t *>(text), uint32_t(std::strlen(text))}, &ref),
                  AYNI_OK);
        return ref;
    }
    void confirm(ayni_body_ref_v1 ref) {
        const auto *store = bodies.store();
        ayni_status_t status = AYNI_NOT_READY;
        ASSERT_TRUE(until(
            [&] { return (status = store->confirm(store->context, &ref)) != AYNI_NOT_READY; }));
        ASSERT_EQ(status, AYNI_OK);
    }
    ayni::test::TempDirectory directory;
    ayni::hosts::native::PosixFiles files;
    Values values{files};
    FileBodies bodies, reopened;
};
// C-HVAL-009: shared-owner removal waits for its manifest CAS; another owner's bytes survive.
TEST_F(ScopedBodyCleanup, shared_owner_release_has_an_independent_durable_completion) {
    const auto first = stage(1, "shared");
    const auto *store = bodies.store();
    ayni_body_ref_v1 second{};
    ASSERT_EQ(store->share(store->context, 2, &first, &second), AYNI_OK);
    confirm(first);
    confirm(second);
    store->release(store->context, &first);
    EXPECT_EQ(FileBodies::collected(&bodies, 1), AYNI_BUSY);
    ASSERT_TRUE(until([&] { return FileBodies::collected(&bodies, 1) == AYNI_OK; }));
    std::array<uint8_t, 6> out{};
    EXPECT_EQ(store->read(store->context, &second, out.data(), out.size()), AYNI_OK);
    const auto unrelated = stage(3, "new unrelated body");
    EXPECT_EQ(store->confirm(store->context, &unrelated), AYNI_NOT_READY);
    EXPECT_FALSE(bodies.idle());
    EXPECT_EQ(FileBodies::collected(&bodies, 1), AYNI_OK);
}
// C-HVAL-009: queue backpressure cannot let a temporary alias discard a cold release fence.
TEST_F(ScopedBodyCleanup, temporary_release_preserves_cold_cleanup_under_queue_backpressure) {
    const auto first = stage(1, "shared");
    confirm(first);
    const auto *store = reopened.store();
    ayni_body_ref_v1 temporary{};
    ASSERT_EQ(store->stage(store->context, 2, {reinterpret_cast<const uint8_t *>("shared"), 6},
                           &temporary),
              AYNI_OK);
    FileValueRequest occupied;
    occupied.key = {99, 1};
    occupied.complete = [](void *, const FileValueResult &) noexcept {};
    const auto port = values.port();
    for (uint32_t at = 0; at < portable::value_request_capacity; ++at) {
        ASSERT_EQ(port.submit(port.context, &occupied), AYNI_SUBMIT_ACCEPTED);
    }
    store->release(store->context, &first);
    store->release_temporary(store->context, &temporary);
    EXPECT_EQ(FileBodies::collected(&reopened, 1), AYNI_BUSY);
    constexpr uint32_t limit = 10000;
    for (uint32_t at = 0; at < limit && !reopened.idle(); ++at) {
        FileBodies::progress(&reopened, 0);
        values.advance();
    }
    EXPECT_TRUE(reopened.idle());
    EXPECT_EQ(FileBodies::collected(&reopened, 1), AYNI_OK);
    std::array<uint8_t, 6> out{};
    EXPECT_EQ(store->read(store->context, &first, out.data(), out.size()), AYNI_NOT_READY);
    for (uint32_t at = 0; at < limit && !reopened.idle(); ++at) {
        FileBodies::progress(&reopened, 0);
        values.advance();
    }
    EXPECT_EQ(store->read(store->context, &first, out.data(), out.size()), AYNI_NOT_FOUND);
}
} // namespace
