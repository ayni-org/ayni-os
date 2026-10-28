// White-box E4 media corruption only; preserve record and batch owners' codecs.
#include "durability/batch/batch_codec.h"
#include "experiment_composite.h"
namespace ayni::runtime::experiments {
bool replace_storage_version(World &world, VersionFault fault, uint16_t version) {
    auto &e = world.evidence;
    if (!e.equal("schema injection requires absent Kernel", world.node.kernel() == nullptr, 1)) {
        return false;
    }
    auto image = world.node.simulation()->durable_storage();
    bool changed_record = false;
    for (auto &stored : image.log) {
        durability::BatchView batch{};
        if (!e.equal("decode source with owner codec",
                     durability::read_batch(stored.bytes.data(), stored.bytes.size(), batch) ==
                         durability::BatchCheck::ok,
                     1)) {
            return false;
        }
        std::vector<uint8_t> bytes(stored.bytes.size());
        durability::BatchWriter writer(bytes.data(), bytes.size(), batch.identity,
                                       fault == VersionFault::schema ? version : batch.schema);
        durability::RecordReader records(batch);
        durability::RecordHeader header{};
        const uint8_t *body = nullptr;
        while (records.next(header, body) == durability::RecordReader::Next::record) {
            if (fault == VersionFault::required_record && !changed_record &&
                header.type == durability::record_type::task_state) {
                header.version = version;
                header.flags |= durability::record_required;
                changed_record = true;
            }
            if (!e.equal("encode original versioned record", writer.add_record(header, body), 1)) {
                return false;
            }
        }
        bytes.resize(writer.finish());
        stored.bytes = std::move(bytes);
    }
    if (fault == VersionFault::required_record &&
        !e.equal("required record version actually changed", changed_record, 1)) {
        return false;
    }
    e.fact("fault", "unsupported_storage_version", version);
    world.node.simulation()->durable_storage() = std::move(image);
    return true;
}
} // namespace ayni::runtime::experiments
