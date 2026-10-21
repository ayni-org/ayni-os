// The Hosts that provide the log ports (C-HOST-009), as the shared storage
// contract suite (C-HOST-017) drives them: through their ayni_host_v1 table,
// plus what a test does to a Host from outside, such as ending an instance,
// cutting power or damaging the medium. Requests go through the kernel's own
// completion slots (white-box CompletionPool), so that a test chooses the
// batches, their commit_seq and the read buffers.

#ifndef AYNI_TESTS_UNIT_STORAGE_HOSTS_H
#define AYNI_TESTS_UNIT_STORAGE_HOSTS_H

#include "kernel/completion/completion_pool.h"
#include "temp_directory.h"

#include <ayni/ayni.h>
#include <ayni/hosts/native.h>
#include <ayni/hosts/sim.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ayni::test {

using Bytes = std::vector<uint8_t>;

// Requests a Host instance must be able to queue at once.
constexpr uint32_t storage_slot_count = 16;

// The encoded bytes of a test batch; the Host does not interpret them.
inline Bytes test_batch(uint64_t commit_seq, std::size_t size) {
    Bytes bytes(size);
    for (std::size_t index = 0; index < size; ++index) {
        bytes[index] = static_cast<uint8_t>(commit_seq * 37U + index);
    }
    return bytes;
}

inline Bytes joined(std::initializer_list<Bytes> parts) {
    Bytes all;
    for (const Bytes &part : parts) {
        all.insert(all.end(), part.begin(), part.end());
    }
    return all;
}

// A Host under test. table() stays valid until the next start().
class StorageHost {
  public:
    virtual ~StorageHost() = default;

    virtual ayni_host_v1 table() = 0;
    // Owner thread. Lets accepted requests progress; false when nothing was
    // posted (sim) or no wake came within seconds (native).
    virtual bool serve() = 0;
    // Ends this instance as a shutdown or a crash does: the native Host
    // answers what it accepted and stops its thread; the sim Host loses it,
    // as at a power cut. Owner thread, before the kernel is destroyed.
    virtual void stop() = 0;
    // A new instance over the same durable log, after stop().
    virtual void start() = 0;
    // Power fails while the Host writes the append issued last, which ends
    // the instance. way picks one of the states this Host's medium can be
    // left in; false once way is past them. The sim Host has one: half the
    // batch (CutAppend::torn). The native Host has two per byte of the
    // frame: the file cut there, or the file at its full length with the
    // rest of the frame zero, as when the size reached the medium before
    // the data.
    virtual bool cut_power_in_last_append(std::size_t way) = 0;
    // Between stop() and start(): changes the byte at offset in what the Host
    // stores for the batch at index, in log order. False, with nothing
    // changed, once offset is past it.
    virtual bool damage_batch(std::size_t index, std::size_t offset) = 0;
    // Whether the Host's framing catches a damaged batch and fails the read,
    // rather than returning the batch as stored for the kernel to check.
    virtual bool checks_frames() const = 0;
};

class SimStorage final : public StorageHost {
  public:
    SimStorage() : sim_(std::make_unique<hosts::sim::Host>()) {}

    ayni_host_v1 table() override { return sim_->table(); }
    bool serve() override { return sim_->complete_storage() > 0U; }
    void stop() override { sim_->power_cut(hosts::sim::CutAppend::lost); }

    void start() override {
        std::vector<hosts::sim::StoredBatch> log = std::move(sim_->log());
        sim_ = std::make_unique<hosts::sim::Host>();
        sim_->log() = std::move(log);
    }

    bool cut_power_in_last_append(std::size_t way) override {
        if (way > 0U) {
            return false;
        }
        sim_->power_cut(hosts::sim::CutAppend::torn);
        return true;
    }

    bool damage_batch(std::size_t index, std::size_t offset) override {
        Bytes &bytes = sim_->log().at(index).bytes;
        if (offset >= bytes.size()) {
            return false;
        }
        bytes[offset] ^= 0xFFU;
        return true;
    }

    bool checks_frames() const override { return false; }

  private:
    std::unique_ptr<hosts::sim::Host> sim_;
};

// Each instance opens the same file in a directory of its own, which goes
// with the object.
class NativeFileStorage final : public StorageHost {
  public:
    // Where a frame lies in the log file (hosts/native/file_log.h): after the
    // 8-byte file header, each frame is a 20-byte header whose bytes 4..7
    // hold the payload length, the payload and a 4-byte CRC.
    struct Extent {
        std::size_t start;
        std::size_t size;
    };

    NativeFileStorage() { start(); }

    ayni_host_v1 table() override { return native_->table(); }
    bool serve() override { return native_->wait_for_wake(std::chrono::seconds(5)); }
    void stop() override { native_->close_storage(); }

    void start() override {
        native_ = std::make_unique<hosts::native::Host>();
        const hosts::native::StorageOpen opened =
            native_->open_storage(directory_.path().c_str(), storage_slot_count);
        EXPECT_EQ(opened.status, hosts::native::StorageStatus::ok) << "errno " << opened.error;
    }

    // The append completes as the instance stops, so its frame is the file's
    // last; what is lost of that frame is what a power cut before its sync
    // could lose.
    bool cut_power_in_last_append(std::size_t way) override {
        stop();
        Bytes file = read_file();
        const std::vector<Extent> frames = frames_of(file);
        if (frames.empty() || way >= 2U * frames.back().size) {
            return false;
        }
        const Extent last = frames.back();
        if (way < last.size) {
            std::filesystem::resize_file(log_path(), last.start + way);
        } else {
            const auto lost_from = static_cast<std::ptrdiff_t>(last.start + (way - last.size));
            std::fill(file.begin() + lost_from, file.end(), uint8_t{0});
            write_file(file);
        }
        return true;
    }

    bool damage_batch(std::size_t index, std::size_t offset) override {
        Bytes file = read_file();
        const std::vector<Extent> frames = frames_of(file);
        if (index >= frames.size() || offset >= frames[index].size) {
            return false;
        }
        file[frames[index].start + offset] ^= 0xFFU;
        write_file(file);
        return true;
    }

    bool checks_frames() const override { return true; }

    hosts::native::Host &native() { return *native_; }
    std::string log_path() const { return directory_.file(hosts::native::Host::log_file_name); }

    Bytes read_file() const {
        std::ifstream in(log_path(), std::ios::binary);
        return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }

    void write_file(const Bytes &bytes) const {
        std::ofstream out(log_path(), std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char *>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }

    static std::vector<Extent> frames_of(const Bytes &file) {
        std::vector<Extent> frames;
        for (std::size_t at = 8; at + 20 <= file.size();) {
            uint32_t length = 0;
            for (std::size_t index = 0; index < 4U; ++index) {
                length |= static_cast<uint32_t>(file[at + 4U + index]) << (8U * index);
            }
            frames.push_back({at, 24U + length});
            at += 24U + length;
        }
        return frames;
    }

  private:
    TempDirectory directory_;
    // After directory_, so that the file is closed before it goes.
    std::unique_ptr<hosts::native::Host> native_;
};

// One request issued through the completion slots, and what came back.
struct StorageRequest {
    bool append = false;
    uint64_t commit_seq = 0; // the batch's, or from_commit_seq
    const ayni_host_v1 *host = nullptr;
    Bytes memory; // the batch, or the read buffer: the kernel's until applied
    ayni_attempt_ref_v1 attempt{};
    kernel::IssueResult issued = kernel::IssueResult::busy;
    int applied = 0;
    ayni_completion_result_v1 result{};
    Bytes received; // what a read returned
};

// The kernel's side of the storage ports. The slots live in memory of their
// own that outlives Host instances, so a new instance's requests get the
// slots and the attempts of an earlier one's, as a reboot reuses the
// kernel's memory (C-ID-006). Requests use boot_epoch 0, as recovery does.
class StorageRequests {
  public:
    explicit StorageRequests(StorageHost &host)
        : host_(host),
          memory_(kernel::CompletionPool::storage_size(storage_slot_count) / sizeof(uint64_t) +
                  1U) {
        reset();
    }

    // The slots go with this object, so the Host instance must not post to
    // them afterwards (C-HOST-018).
    ~StorageRequests() { host_.stop(); }

    StorageRequests(const StorageRequests &) = delete;
    StorageRequests &operator=(const StorageRequests &) = delete;

    // Slots for a new kernel over the current Host instance, after stop()
    // and start(); the requests of the earlier one are forgotten.
    void reset() {
        pool_.reset();
        requests_.clear();
        table_ = host_.table();
        pool_.emplace(memory_.data(), storage_slot_count, table_);
    }

    StorageRequest &append(uint64_t commit_seq, Bytes batch) {
        StorageRequest request;
        request.append = true;
        request.commit_seq = commit_seq;
        request.memory = std::move(batch);
        return issue(std::move(request), &StorageRequests::call_append);
    }

    StorageRequest &read(uint64_t from_commit_seq, uint32_t buffer_size) {
        StorageRequest request;
        request.commit_seq = from_commit_seq;
        request.memory.assign(buffer_size, 0);
        return issue(std::move(request), &StorageRequests::call_read);
    }

    // Serves the Host and applies results until no request is in flight.
    void finish() {
        for (int round = 0; round < 1000 && pool_->in_flight() > 0U; ++round) {
            const bool served = host_.serve();
            if (pool_->apply_posted(storage_slot_count) == 0U && !served) {
                break;
            }
        }
        EXPECT_EQ(pool_->in_flight(), 0U) << "requests still in flight";
    }

    // read(), finish(), and the bytes if the read was OK.
    std::optional<Bytes> read_now(uint64_t from_commit_seq, uint32_t buffer_size = 4096) {
        StorageRequest &request = read(from_commit_seq, buffer_size);
        finish();
        if (request.applied != 1 || request.result.outcome != AYNI_WRITE_OK) {
            return std::nullopt;
        }
        return request.received;
    }

    // append() and finish(); whether it was OK.
    bool append_now(uint64_t commit_seq, Bytes batch) {
        StorageRequest &request = append(commit_seq, std::move(batch));
        finish();
        return request.applied == 1 && request.result.outcome == AYNI_WRITE_OK;
    }

    const kernel::CompletionPool &pool() const { return *pool_; }

  private:
    StorageRequest &issue(StorageRequest request, kernel::PortCall call) {
        request.host = &table_;
        requests_.push_back(std::move(request));
        StorageRequest &issued = requests_.back();
        issued.issued = pool_->issue(0, &StorageRequests::on_result, &issued, call, &issued);
        return issued;
    }

    static ayni_submit_result_t call_append(void *context, ayni_attempt_ref_v1 attempt,
                                            ayni_completion_t *completion) {
        StorageRequest &request = *static_cast<StorageRequest *>(context);
        request.attempt = attempt;
        ayni_storage_batch_v1 batch{};
        batch.struct_size = sizeof(batch);
        batch.writer_epoch = 1;
        batch.transition_id = request.commit_seq;
        batch.commit_seq = request.commit_seq;
        batch.encoded.data = request.memory.data();
        batch.encoded.size = static_cast<uint32_t>(request.memory.size());
        return request.host->storage_append(request.host->ctx, &batch, attempt, completion);
    }

    static ayni_submit_result_t call_read(void *context, ayni_attempt_ref_v1 attempt,
                                          ayni_completion_t *completion) {
        StorageRequest &request = *static_cast<StorageRequest *>(context);
        request.attempt = attempt;
        return request.host->storage_read_prefix(
            request.host->ctx, request.commit_seq, request.memory.data(),
            static_cast<uint32_t>(request.memory.size()), attempt, completion);
    }

    static void on_result(void *context, const ayni_completion_result_v1 &result) {
        StorageRequest &request = *static_cast<StorageRequest *>(context);
        ++request.applied;
        request.result = result;
        if (!request.append && result.outcome == AYNI_WRITE_OK &&
            result.value <= request.memory.size()) {
            request.received.assign(request.memory.begin(),
                                    request.memory.begin() +
                                        static_cast<std::ptrdiff_t>(result.value));
        }
        // The Host no longer owns this memory (C-HOST-019): overwriting it
        // races with a late access under TSan, and releasing it turns one
        // into a use after free under ASan.
        std::fill(request.memory.begin(), request.memory.end(), uint8_t{0xA5});
        Bytes().swap(request.memory);
    }

    StorageHost &host_;
    std::vector<uint64_t> memory_;
    ayni_host_v1 table_{};
    std::optional<kernel::CompletionPool> pool_;
    // A deque, so that the requests in flight keep their addresses.
    std::deque<StorageRequest> requests_;
};

} // namespace ayni::test

#endif // AYNI_TESTS_UNIT_STORAGE_HOSTS_H
