// White-box E4 media corruption only; preserve record and batch owners' codecs.
#include "durability/batch/batch_codec.h"
#include "foundation/crc32.h"
#include "foundation/little_endian.h"
#include "experiment_composite.h"
namespace ayni::runtime::experiments {
bool replace_storage_version(World &world, VersionFault fault, uint16_t version) {
    auto &e = world.evidence;
    if (!e.equal("schema injection requires absent Kernel", world.node.kernel() == nullptr, 1)) {
        return false;
    }
    auto image = world.node.simulation()->durable_storage();
    world.evidence.fact("writer", "storage",
                        fault == VersionFault::schema ? version : durability::storage_schema);
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
        durability::BatchWriter writer(bytes.data(), bytes.size(), batch.identity);
        durability::RecordReader records(batch);
        durability::RecordHeader header{};
        const uint8_t *body = nullptr;
        auto next = records.next(header, body);
        while (next == durability::RecordReader::Next::record) {
            if (fault == VersionFault::required_record && !changed_record &&
                header.type == durability::record_type::task_state) {
                header.version = version;
                header.flags |= durability::record_required;
                changed_record = true;
            }
            if (!e.equal("encode original versioned record", writer.add_record(header, body), 1)) {
                return false;
            }
            next = records.next(header, body);
        }
        if (!e.equal("version source record framing complete",
                     next == durability::RecordReader::Next::end, 1)) {
            return false;
        }
        const auto size = writer.finish();
        if (!e.equal("version rewrite encoded complete batch", size != 0, 1)) {
            return false;
        }
        bytes.resize(size);
        if (fault == VersionFault::schema) {
            constexpr uint32_t schema_at = 4;
            constexpr uint32_t crc_at = durability::batch_header_size - sizeof(uint32_t);
            foundation::store_le16(bytes.data() + schema_at, version);
            foundation::store_le32(bytes.data() + crc_at, foundation::crc32(bytes.data(), crc_at));
        }
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
