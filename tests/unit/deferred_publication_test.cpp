// ADR-0113: portable verification of PublicationCollection::deferred and Session::preview_answer():
// 1. preview_answer() returns the completed answer text immediately at checkpoint_ready (0 ms RAM
//    preview) while TurnResult::published stays false until CONTEXT_ACK is durable.
// 2. Ordinary publication under PublicationCollection::deferred finishes CONTEXT_ACK and reaches
//    Session::ready() without blocking on slow superseded-body collection (collect_busy_remaining).
// 3. Save::erasure (forget) and Session::quiesce() still strictly wait for collected() == AYNI_OK
//    before ACK / clean shutdown.

#include <ayni/hosts/sim.h>
#include <ayni/hosts/storage/file_value.h>
#include <ayni/runtime/portable.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <deque>
#include <map>
#include <new>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace ayni::runtime;
using namespace ayni::hosts::storage;

ayni_string_v1 text(const char *s) {
    return {s, static_cast<uint32_t>(std::strlen(s))};
}

class InMemoryFiles final : public Files {
  public:
    bool contains(std::string_view bytes) const {
        for (const auto &[name, data] : files_) {
            if (std::search(data.begin(), data.end(), bytes.begin(), bytes.end()) != data.end()) {
                return true;
            }
        }
        return false;
    }

    int begin(const char *) override { return 0; }
    void end() override { handles_.clear(); }
    int open(const char *name, Mode mode, int &handle) override {
        const std::string key(name);
        auto it = files_.find(key);
        if (mode == Mode::read_existing || mode == Mode::read_existing_no_follow) {
            if (it == files_.end()) {
                return ENOENT;
            }
        } else if (mode == Mode::replace) {
            files_[key].clear();
        } else {
            files_[key];
        }
        handle = next_handle_++;
        handles_[handle] = key;
        return 0;
    }
    void close(int handle) override { handles_.erase(handle); }
    int lock(int) override { return 0; }
    int read(int handle, uint64_t offset, uint8_t *data, std::size_t bytes,
             std::size_t &count) const override {
        count = 0;
        const auto h = handles_.find(handle);
        if (h == handles_.end()) {
            return EBADF;
        }
        const auto f = files_.find(h->second);
        if (f == files_.end() || offset >= f->second.size()) {
            return 0;
        }
        count = std::min<std::size_t>(bytes, f->second.size() - static_cast<std::size_t>(offset));
        std::memcpy(data, f->second.data() + offset, count);
        return 0;
    }
    int write(int handle, uint64_t offset, const uint8_t *data, std::size_t bytes) override {
        const auto h = handles_.find(handle);
        if (h == handles_.end()) {
            return EBADF;
        }
        auto &buf = files_[h->second];
        const auto end = static_cast<std::size_t>(offset) + bytes;
        if (buf.size() < end) {
            buf.resize(end);
        }
        if (bytes != 0U) {
            std::memcpy(buf.data() + offset, data, bytes);
        }
        return 0;
    }
    int size(int handle, uint64_t &bytes) const override {
        const auto h = handles_.find(handle);
        if (h == handles_.end()) {
            return EBADF;
        }
        const auto f = files_.find(h->second);
        bytes = f == files_.end() ? 0 : f->second.size();
        return 0;
    }
    int truncate(int handle, uint64_t bytes) override {
        const auto h = handles_.find(handle);
        if (h == handles_.end()) {
            return EBADF;
        }
        files_[h->second].resize(static_cast<std::size_t>(bytes));
        return 0;
    }
    int sync_file(int) override { return 0; }
    int sync_namespace() override { return 0; }
    int rename(const char *from, const char *to) override {
        auto it = files_.find(from);
        if (it == files_.end()) {
            return ENOENT;
        }
        files_[to] = std::move(it->second);
        files_.erase(it);
        return 0;
    }
    int remove(const char *name) override { return files_.erase(name) ? 0 : ENOENT; }
    int free_bytes(uint64_t &bytes) const override {
        bytes = 4U * 1024U * 1024U;
        return 0;
    }

  private:
    std::map<std::string, std::vector<uint8_t>> files_;
    std::map<int, std::string> handles_;
    int next_handle_ = 1;
};

class QueuedValues {
  public:
    explicit QueuedValues(Files &files) : files_(files) {}
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
    static ayni_submit_result_t submit(void *ctx, const FileValueRequest *request) {
        auto &self = *static_cast<QueuedValues *>(ctx);
        if (self.pending_.size() == portable::value_request_capacity) {
            return AYNI_SUBMIT_REJECTED_NOT_RETAINED;
        }
        self.pending_.push_back(*request);
        return AYNI_SUBMIT_ACCEPTED;
    }
    Files &files_;
    std::deque<FileValueRequest> pending_;
};

class DeferredPublicationTest : public testing::Test {
  protected:
    void SetUp() override {
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

        inner_collection = static_cast<FileBodies *>(profile.bodies()->context)->collection();
        auto wrapped = inner_collection;
        wrapped.context = this;
        wrapped.collect = [](void *ctx, uint64_t scope, const std::vector<ayni_body_ref_v1> &keep) {
            auto &s = *static_cast<DeferredPublicationTest *>(ctx);
            ++s.collect_calls;
            s.inner_collection.collect(s.inner_collection.context, scope, keep);
        };
        wrapped.ready = [](void *ctx, uint64_t task) {
            auto &s = *static_cast<DeferredPublicationTest *>(ctx);
            return s.inner_collection.ready(s.inner_collection.context, task);
        };
        wrapped.progress = [](void *ctx, uint64_t task) {
            auto &s = *static_cast<DeferredPublicationTest *>(ctx);
            return s.inner_collection.progress(s.inner_collection.context, task);
        };
        wrapped.status = [](void *ctx) {
            auto &s = *static_cast<DeferredPublicationTest *>(ctx);
            return s.inner_collection.status(s.inner_collection.context);
        };
        wrapped.collected = [](void *ctx, uint64_t scope) {
            auto &s = *static_cast<DeferredPublicationTest *>(ctx);
            if (s.block_collected) {
                ++s.collected_busy_hits;
                return AYNI_BUSY;
            }
            return s.inner_collection.collected(s.inner_collection.context, scope);
        };
        conversation::Config cfg{};
        cfg.kernel = profile.kernel();
        cfg.session = profile.session();
        cfg.owner_instance = profile.instance(0);
        cfg.goal_type = text("portable.chat.a");
        cfg.workspace = profile.value_workspace();
        cfg.files = values.port();
        cfg.bodies = wrapped;
        cfg.commands = {this, command};
        cfg.unfinished = conversation::UnfinishedTurn::discard;
        cfg.persistence = persistence;
        cfg.observer = {this, on_stage, nullptr};
        cfg.collection_mode = conversation::PublicationCollection::deferred;
        ASSERT_EQ(session.configure(cfg), AYNI_OK);
        bound = true;
        ASSERT_TRUE(until([&] { return session.ready(); }));
    }

    void TearDown() override {
        block_collected = false;
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
        for (uint32_t tick = 0; tick < 20000; ++tick) {
            if (done()) {
                return true;
            }
            const auto p = profile.tick();
            if (p != AYNI_OK && p != AYNI_BUSY && p != AYNI_NOT_READY) {
                ADD_FAILURE() << "profile=" << p;
                return false;
            }
            if (bound) {
                bool again = false;
                const auto s = session.poll_with_progress(again);
                if (s != AYNI_OK && s != AYNI_BUSY && s != AYNI_NOT_READY) {
                    ADD_FAILURE() << "session=" << s;
                    return false;
                }
            }
            values.advance();
            host.complete_storage();
            host.advance_us(1000);
        }
        return false;
    }

    static ayni::Submission command(void *ctx, ayni_intent_kind_t kind, const void *body,
                                    uint32_t revision) {
        return static_cast<DeferredPublicationTest *>(ctx)->profile.submit(kind, body, revision);
    }

    static void on_stage(void *ctx, uint64_t, uint64_t, conversation::PublicationStage stage) {
        auto &self = *static_cast<DeferredPublicationTest *>(ctx);
        self.stages.push_back(stage);
        if (stage == conversation::PublicationStage::checkpoint_ready) {
            // 0 ms RAM preview works at checkpoint_ready before durable CONTEXT_ACK.
            self.preview_status = self.session.preview_answer(self.preview_text);
            self.pending_result_at_preview = self.session.pending_result() != nullptr;
        }
    }

    ayni::strategies::Persistence persistence = ayni::strategies::Persistence::turn_boundary;
    InMemoryFiles files;
    QueuedValues values{files};
    ayni::hosts::sim::Host host;
    portable::Memory memory{};
    void *arena = nullptr;
    portable::Profile profile;
    BodyCollection inner_collection{};
    conversation::Session session;
    bool bound = false;
    bool block_collected = false;
    uint32_t collect_calls = 0;
    uint32_t collected_busy_hits = 0;
    std::vector<conversation::PublicationStage> stages;
    ayni_status_t preview_status = AYNI_NOT_FOUND;
    std::string preview_text;
    bool pending_result_at_preview = true;
};

// ADR-0113: deferred publication delivers 0 ms preview at checkpoint_ready, completes CONTEXT_ACK
// without waiting on blocked collection, and still enforces strict collection on erasure/quiesce.
TEST_F(DeferredPublicationTest,
       deferred_gc_unblocks_publication_and_preview_answer_precedes_ack_while_erasure_stays_sync) {
    // Keep collected() returning BUSY throughout ordinary publication.
    block_collected = true;
    ASSERT_EQ(session.begin(text("hello deferred")), AYNI_OK);
    ASSERT_TRUE(until([&] { return session.ready(); }));

    // 1. preview_answer succeeded at checkpoint_ready BEFORE TurnResult::published became ready.
    EXPECT_EQ(preview_status, AYNI_OK);
    EXPECT_FALSE(preview_text.empty());
    EXPECT_FALSE(pending_result_at_preview);

    // 2. Ordinary publication completed CONTEXT_ACK with collected stage omitted (deferred GC).
    conversation::TurnResult result{};
    ASSERT_TRUE(session.take_result(result));
    EXPECT_TRUE(result.published);
    EXPECT_EQ(collect_calls, 1U);
    ASSERT_EQ(stages.size(), 4U);
    EXPECT_EQ(stages[0], conversation::PublicationStage::checkpoint_ready);
    EXPECT_EQ(stages[1], conversation::PublicationStage::publish_durable);
    EXPECT_EQ(stages[2], conversation::PublicationStage::record_saved);
    EXPECT_EQ(stages[3], conversation::PublicationStage::context_ready);

    // Exact saved answer matches the early preview text byte-for-byte.
    std::string saved;
    ASSERT_EQ(
        ayni::strategies::Conversation::latest_answer(*profile.bodies(), *session.record(), saved),
        AYNI_OK);
    EXPECT_EQ(saved, preview_text);
    // preview_answer returns NOT_FOUND once publication has settled back to ready().
    std::string ignored;
    EXPECT_EQ(session.preview_answer(ignored), AYNI_NOT_FOUND);

    // 3. quiesce() still enforces drainage while block_collected is true.
    EXPECT_EQ(session.quiesce(), AYNI_BUSY);

    // 4. Privacy erasure (Save::erasure) MUST still synchronously wait for collected() == AYNI_OK.
    collected_busy_hits = 0;
    ASSERT_EQ(session.forget(AYNI_CONTEXT_ERASE_ALL), AYNI_OK);
    ASSERT_TRUE(until([&] { return collected_busy_hits >= 5U; }));
    EXPECT_FALSE(session.ready()); // Erasure refuses to ACK while collection is still BUSY.

    // Unblocking collection lets erasure ACK and quiesce() succeed cleanly.
    block_collected = false;
    ASSERT_TRUE(until([&] { return session.ready(); }));
    EXPECT_EQ(session.record()->round_count, 0U);
    ASSERT_TRUE(until([&] { return session.quiesce() == AYNI_OK; }));
}

// ADR-0111/0113: volatile discard during a cold-cache turn retries Conversation::references()
// asynchronously and reclaims the cancelled draft so FileBodies never leaks staged RAM.
TEST_F(DeferredPublicationTest,
       volatile_discard_after_cold_cache_trim_retries_cleanup_and_reclaims_draft) {
    // Seed 1 durable turn so current_.history and current_.rounds are populated on disk.
    ASSERT_EQ(session.begin(text("first durable turn")), AYNI_OK);
    ASSERT_TRUE(until([&] { return session.ready(); }));
    conversation::TurnResult r1{};
    ASSERT_TRUE(session.take_result(r1));
    ASSERT_TRUE(r1.published);
    ASSERT_EQ(session.record()->round_count, 1U);
    ASSERT_TRUE(until([&] { return session.quiesce() == AYNI_OK; }));

    // Evict all cached payloads so Conversation::references() returns AYNI_NOT_READY on discard.
    auto *bodies = static_cast<FileBodies *>(profile.bodies()->context);
    ASSERT_EQ(bodies->trim_cache(), AYNI_OK);
    ASSERT_EQ(bodies->cached_bytes(), 0U);

    // Stage a brand-new draft and immediately cancel while history/rounds are evicted.
    ASSERT_EQ(session.begin(text("cold cancelled turn text")), AYNI_OK);
    ASSERT_EQ(session.cancel(), AYNI_OK);
    ASSERT_TRUE(until([&] { return session.ready(); }));
    conversation::TurnResult r2{};
    ASSERT_TRUE(session.take_result(r2));
    EXPECT_FALSE(r2.published);
    EXPECT_EQ(session.record()->round_count, 1U);

    // The next turn may stage input while cleanup is cold. Its Task must not
    // create bodies in the scope until the old keep set has been queued.
    const auto collection_before = collect_calls;
    ASSERT_EQ(session.begin(text("new turn after cancellation")), AYNI_OK);
    ASSERT_EQ(session.poll(), AYNI_BUSY);
    EXPECT_EQ(collect_calls, collection_before);
    EXPECT_EQ(session.latest_goal(), 0U);
    ASSERT_TRUE(until([&] { return session.ready(); }));
    conversation::TurnResult next{};
    ASSERT_TRUE(session.take_result(next));
    EXPECT_TRUE(next.published);
    EXPECT_EQ(session.record()->round_count, 2U);

    ASSERT_TRUE(until([&] { return session.quiesce() == AYNI_OK; }));
    EXPECT_EQ(bodies->trim_cache(), AYNI_OK);
    EXPECT_EQ(bodies->cached_bytes(), 0U);
}

// ADR-0113: a RAM preview still needs the live Core privacy anchor.
TEST_F(DeferredPublicationTest, preview_refuses_after_core_accepts_forget) {
    ASSERT_EQ(session.begin(text("private prompt")), AYNI_OK);
    ASSERT_TRUE(until([&] { return preview_status == AYNI_OK; }));
    auto anchor = ayni::initialized<ayni_context_info_v1>();
    ASSERT_EQ(ayni_get_context(profile.kernel(), profile.session(), profile.instance(0), &anchor),
              AYNI_OK);
    auto forget = ayni::initialized<ayni_context_command_v1>();
    forget.action = AYNI_CONTEXT_ERASE_ALL;
    forget.context_id = anchor.context_id;
    forget.expected_generation = anchor.generation;
    ASSERT_EQ(profile.submit(AYNI_INTENT_CONTEXT_FORGET, &forget, anchor.revision).status, AYNI_OK);
    std::string after_forget;
    EXPECT_NE(session.preview_answer(after_forget), AYNI_OK);
    EXPECT_TRUE(after_forget.empty());
}

class TurnBoundaryPublicationTest : public DeferredPublicationTest {
  protected:
    TurnBoundaryPublicationTest() { persistence = ayni::strategies::Persistence::turn_boundary; }
};

// C-HVAL-009 / ADR-0113: retain a complete staged graph across a model/tool round,
// and persist its history only when Session accepts the completed candidate.
TEST_F(TurnBoundaryPublicationTest, draft_and_intermediate_indexes_wait_for_publication) {
    constexpr auto user = "three sums";
    constexpr auto history_magic = "AYNDXI1";
    ASSERT_EQ(session.begin(text(user)), AYNI_OK);
    ASSERT_TRUE(until([&] { return preview_status == AYNI_OK; }));
    EXPECT_EQ(profile.model_submissions(), 4U);
    EXPECT_FALSE(files.contains(user));
    EXPECT_FALSE(files.contains(history_magic));

    conversation::TurnResult result{};
    ASSERT_TRUE(until([&] { return session.take_result(result); }));
    EXPECT_TRUE(result.published);
    EXPECT_TRUE(files.contains(user));
    EXPECT_TRUE(files.contains(history_magic));
    ASSERT_TRUE(until([&] { return session.quiesce() == AYNI_OK; }));
    auto *bodies = static_cast<FileBodies *>(profile.bodies()->context);
    ASSERT_EQ(bodies->trim_cache(), AYNI_OK);

    ASSERT_EQ(session.begin(text("read cold published history")), AYNI_OK);
    ASSERT_TRUE(until([&] { return session.take_result(result); }));
    EXPECT_TRUE(result.published);
    EXPECT_EQ(session.record()->round_count, 2U);

    ASSERT_EQ(session.forget(AYNI_CONTEXT_ERASE_ALL), AYNI_OK);
    ASSERT_TRUE(until([&] { return session.ready() && session.quiesce() == AYNI_OK; }));
    EXPECT_FALSE(files.contains(user));
    EXPECT_FALSE(files.contains(history_magic));
}

// ADR-0111/0113: a draft discarded before dispatch never becomes a saved input.
TEST_F(TurnBoundaryPublicationTest, cancelling_staged_input_leaves_no_body_file) {
    constexpr auto input = "cancel a staged input before dispatch";
    ASSERT_EQ(session.begin(text(input)), AYNI_OK);
    ASSERT_EQ(session.cancel(), AYNI_OK);
    ASSERT_TRUE(until([&] { return session.ready() && session.quiesce() == AYNI_OK; }));
    EXPECT_FALSE(files.contains(input));
    EXPECT_EQ(profile.model_submissions(), 0U);
    EXPECT_EQ(session.record()->round_count, 0U);
}

} // namespace
