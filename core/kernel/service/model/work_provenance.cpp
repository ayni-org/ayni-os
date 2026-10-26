// Kernel composes work/knowledge source retention with Grant's durable unlink fence.
#include "kernel/service/model/work_model.h"
namespace ayni::kernel {
void WorkModel::release_content_references(const knowledge::MemoryDependencies &dependencies,
                                           uint64_t commit_seq) {
    for (uint32_t i = 0; i < dependencies.count; ++i) {
        const auto &ref = dependencies.items[i];
        grants.release_reference(ref.read.id, commit_seq);
        grants.release_reference(ref.egress.id, commit_seq);
    }
}
void WorkModel::clear_content_provenance(work::ProvenanceKind kind, uint32_t slot, uint64_t id,
                                         uint64_t commit_seq) {
    const auto *record = provenance.find(kind, slot, id);
    if (record != nullptr) {
        release_content_references(record->dependencies, commit_seq);
    }
    provenance.clear(kind, slot);
}
// Invalid content still retains exact selected Grants until its metadata is durably unlinked;
// this prevents compaction/reclamation from silently replacing a captured permission.
void WorkModel::protect_content_grants(authority::GrantReclaimExclusions &excluded) const {
    const auto protect = [&](const knowledge::MemoryDependencies &dependencies) {
        for (uint32_t i = 0; i < dependencies.count; ++i) {
            const auto &ref = dependencies.items[i];
            uint32_t index = 0;
            if (grants.find(ref.read.id, index)) {
                excluded.add(index);
            }
            if (grants.find(ref.egress.id, index)) {
                excluded.add(index);
            }
        }
    };
    for (uint32_t i = 0; i < memories.size(); ++i) {
        protect(memories.at(i).dependencies);
    }
    for (uint32_t i = 0; i < provenance.capacity(); ++i) {
        if (provenance.at(i).id != 0) {
            protect(provenance.at(i).dependencies);
        }
    }
}
} // namespace ayni::kernel
