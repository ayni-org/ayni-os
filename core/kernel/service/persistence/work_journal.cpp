#include "kernel/service/persistence/work_journal.h"

#include "authority/grant/grant_records.h"
#include "authority/principal/owner_binding_records.h"
#include "catalog/catalog_id.h"
#include "durability/batch/record_body.h"
#include "durability/journal/journal_ring.h"
#include "kernel/service/command/command_records.h"
#include "kernel/service/model/store_limits.h"
#include "kernel/service/persistence/persistent_store.h"
#include "work/agent/agent_records.h"
#include "work/authority/authority_cleanup_records.h"
#include "work/goal/goal_authority_records.h"
#include "work/goal/goal_records.h"
#include "work/task/task_authority_records.h"
#include "work/task/task_records.h"

namespace ayni::kernel {
namespace {

durability::RecordHeader header(uint16_t type, uint16_t version, uint32_t length) {
    return {type, version, durability::record_required, length};
}

class Encoder {
  public:
    Encoder(const WorkModel &model, foundation::TransitionStamp stamp,
            durability::BatchWriter &writer)
        : model_(model), stamp_(stamp), writer_(writer) {}

    // The first binding precedes the instances it starts. Work records
    // follow replay order, with the command mapping last.
    bool encode() {
        return owner_binding() && stores(StorePosition::instances) &&
               stores(StorePosition::before_work) && stores(StorePosition::goals) &&
               stores(StorePosition::tasks) && stores(StorePosition::after_work) &&
               stores(StorePosition::after_cleanup) && authority_cleanup();
    }

  private:
    // One exact command owns every replacement state/admission in this batch.

    uint8_t *body(uint16_t type, uint16_t version, uint32_t length) {
        uint8_t *at = writer_.add_record_body(header(type, version, length));
        ok_ = ok_ && at != nullptr;
        return at;
    }

    bool stores(StorePosition position) {
        for (const PersistentStore *store : persistent_stores) {
            if (store->position == position) {
                ok_ = ok_ && store->journal(model_, stamp_, writer_);
            }
        }
        return ok_;
    }

    bool owner_binding() {
        const authority::OwnerBinding *binding = model_.principals.binding();
        if (binding == nullptr || binding->stamp.commit_seq != stamp_.commit_seq ||
            binding->stamp.transition_id != stamp_.transition_id) {
            return ok_;
        }
        uint8_t *at =
            body(authority::owner_binding_record_type, authority::owner_binding_record_version,
                 authority::owner_binding_record_size);
        if (at != nullptr) {
            authority::write_owner_binding_record(*binding, at);
        }
        return ok_;
    }

    // Metadata follows the Goal state and shares exactly the authority change's
    // stamp. No live catalog is consulted while serializing durable facts.

    bool authority_cleanup() {
        const auto &record = model_.authority_cleanup.record();
        if (record.stamp.commit_seq != stamp_.commit_seq) {
            return ok_;
        }
        auto *at = body(work::authority_cleanup_record_type, work::authority_cleanup_record_version,
                        work::authority_cleanup_record_size);
        if (at != nullptr) {
            work::write_authority_cleanup_record(record, at);
        }
        return ok_;
    }

    const WorkModel &model_;
    foundation::TransitionStamp stamp_;
    durability::BatchWriter &writer_;
    bool ok_ = true;
};

} // namespace

uint32_t transition_batch_max(const KernelCapacities &capacities) {
    uint32_t size =
        static_cast<uint32_t>(durability::batch_header_size + durability::commit_marker_size);
    size += record_bytes(authority::owner_binding_record_size);
    for (const PersistentStore *store : persistent_stores) {
        size += store->journal_max(capacities);
    }
    size += record_bytes(work::authority_cleanup_record_size);
    return size;
}

uint32_t storage_buffer_min(const KernelCapacities &capacities) {
    return transition_batch_max(capacities) + durability::JournalRing::entry_overhead;
}

std::size_t encode_transition(const WorkModel &model, foundation::TransitionStamp stamp,
                              durability::BatchWriter &writer) {
    Encoder encoder(model, stamp, writer);
    return encoder.encode() ? writer.finish() : 0U;
}

} // namespace ayni::kernel
