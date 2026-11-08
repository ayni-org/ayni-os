#include "history.h"
#include <vector>
namespace wifichat {
namespace {
using ayni::hosts::storage::FileValueRequest;
using ayni::hosts::storage::FileValueResult;
using ayni::strategies::Conversation;
// Conversation record IDs also scope their bodies, separately from Task prompt bodies.
} // namespace
ayni::strategies::ConversationRecord History::fresh(uint64_t generation) const {
    ayni::strategies::ConversationRecord record{};
    record.id = record_id_;
    record.owner_instance_id = owner_;
    record.generation = generation;
    return record;
}
ayni_status_t History::configure(const ayni_workspace_v1 *workspace, BodyCache *bodies,
                                 ayni::hosts::storage::FileValuePort files, uint64_t owner,
                                 uint64_t record_id) {
    if (phase_ != Phase::empty) {
        return AYNI_INVALID_STATE;
    }
    if (!workspace || !bodies || !files.submit || !owner || !record_id) {
        return AYNI_INVALID_ARGUMENT;
    }
    workspace_ = workspace;
    bodies_ = bodies;
    files_ = files;
    owner_ = owner;
    record_id_ = record_id;
    phase_ = Phase::load;
    return AYNI_OK;
}
void History::complete(void *context, const FileValueResult &result) noexcept {
    auto &self = *static_cast<History *>(context);
    self.result_ = result;
    self.completed_.store(true, std::memory_order_release);
}
ayni_status_t History::fail(ayni_status_t status) {
    failure_ = status;
    phase_ = Phase::failed;
    return status;
}
void History::collect() {
    std::vector<ayni_body_ref_v1> keep;
    Conversation::references(current_, keep);
    bodies_->collect(record_id_, keep);
}
// collect() only sees bodies written in this boot; the bodies of a record loaded from storage
// are released by reference once a saved record stops naming them.
void History::release_superseded(const ayni::strategies::ConversationRecord &old) {
    std::vector<ayni_body_ref_v1> before, after;
    Conversation::references(old, before);
    Conversation::references(current_, after);
    const auto *store = bodies_->store();
    for (const auto &ref : before) {
        bool named = false;
        for (const auto &kept : after) {
            named = named || kept.id == ref.id;
        }
        if (!named) {
            store->release(store->context, &ref);
        }
    }
}
ayni_status_t History::loaded() {
    if (result_.status == AYNI_NOT_FOUND) {
        current_ = fresh(1);
        value_version_ = 0;
        phase_ = Phase::ready;
        return AYNI_OK;
    }
    if (result_.status != AYNI_OK) {
        return fail(result_.status);
    }
    ayni::strategies::ConversationRecord record{};
    const auto status =
        Conversation::decode(workspace_, {file_.bytes.data(), result_.size}, record);
    if (status != AYNI_OK) {
        return fail(status);
    }
    // This store never saves a draft; any other record is not this device's conversation.
    if (record.id != record_id_ || record.owner_instance_id != owner_ || record.draft.size) {
        return fail(AYNI_INVALID_STATE);
    }
    current_ = record;
    value_version_ = result_.generation;
    phase_ = Phase::ready;
    return AYNI_OK;
}
ayni_status_t History::saved() {
    if (result_.status != AYNI_OK) {
        return fail(result_.status); // An unknown write is never shown as saved.
    }
    value_version_ = result_.generation;
    const auto old = current_;
    current_ = candidate_;
    release_superseded(old);
    collect(); // Superseded summaries, rounds and the turn's input are no longer named.
    answer_pending_ = turn_save_;
    phase_ = Phase::ready;
    return AYNI_OK;
}
ayni_status_t History::poll() {
    if (phase_ == Phase::empty) {
        return AYNI_INVALID_STATE;
    }
    if (phase_ == Phase::failed) {
        return failure_;
    }
    // Submits body file work a full queue refused earlier, such as the removal a repeated input
    // waits for; nothing else flushes it while no Task is running.
    static_cast<void>(BodyCache::ready(bodies_, 0));
    if (phase_ == Phase::load || phase_ == Phase::save) {
        FileValueRequest request;
        request.key = {owner_, record_id_};
        request.context = this;
        request.complete = complete;
        if (phase_ == Phase::load) {
            request.buffer = file_.bytes.data();
            request.size = static_cast<uint32_t>(file_.bytes.size());
        } else {
            if (value_version_ == UINT64_MAX) {
                return fail(AYNI_CAPACITY_EXCEEDED);
            }
            const auto status = Conversation::encode(workspace_, candidate_, file_);
            if (status != AYNI_OK) {
                return fail(status);
            }
            request.kind = FileValueRequest::Kind::write;
            request.previous_generation = value_version_;
            request.generation = value_version_ + 1;
            request.data = file_.bytes.data();
            request.size = file_.size;
        }
        // A full queue keeps this phase; the next poll submits again.
        if (files_.submit(files_.context, &request) == AYNI_SUBMIT_ACCEPTED) {
            phase_ = phase_ == Phase::load ? Phase::loading : Phase::saving;
        }
        return AYNI_OK;
    }
    if (phase_ == Phase::loading || phase_ == Phase::saving) {
        if (!completed_.exchange(false, std::memory_order_acquire)) {
            return AYNI_OK;
        }
        const auto status = phase_ == Phase::loading ? loaded() : saved();
        if (status != AYNI_OK) {
            return status;
        }
    }
    if (answer_pending_) { // In the same poll as the save, so the answer waits for no wake.
        std::vector<ayni::strategies::Round> rounds;
        const auto status = Conversation::rounds(*bodies_->store(), current_, rounds);
        if (status == AYNI_NOT_READY) {
            return AYNI_OK; // The rounds body is still loading.
        }
        if (status != AYNI_OK || rounds.empty() || rounds.back().number != current_.last_round) {
            return fail(status != AYNI_OK ? status : AYNI_INVALID_STATE);
        }
        answer_ = std::move(rounds.back().answer);
        answer_pending_ = false;
        answer_ready_ = true;
    }
    return AYNI_OK;
}
ayni_status_t History::begin(ayni_string_v1 user, ayni::strategies::Encoded &parameters) {
    if (!ready()) {
        return phase_ == Phase::failed ? failure_ : AYNI_BUSY;
    }
    return Conversation::begin(workspace_, *bodies_->store(), current_, user, parameters);
}
ayni_status_t History::finish(ayni_bytes_v1 checkpoint) {
    if (!ready()) {
        return phase_ == Phase::failed ? failure_ : AYNI_BUSY;
    }
    const auto status = Conversation::finish(workspace_, current_, checkpoint, candidate_);
    if (status != AYNI_OK) {
        return status;
    }
    turn_save_ = true;
    phase_ = Phase::save;
    return AYNI_OK;
}
void History::abandon() {
    if (phase_ == Phase::ready) {
        collect();
    }
}
ayni_status_t History::forget() {
    if (!ready()) {
        return phase_ == Phase::failed ? failure_ : AYNI_BUSY;
    }
    const auto status = Conversation::erase(workspace_, current_, candidate_);
    if (status != AYNI_OK) {
        return status;
    }
    turn_save_ = false;
    phase_ = Phase::save;
    return AYNI_OK;
}
bool History::take_answer(std::string &out) {
    if (!answer_ready_) {
        return false;
    }
    out = std::move(answer_);
    answer_.clear();
    answer_ready_ = false;
    return true;
}
} // namespace wifichat
