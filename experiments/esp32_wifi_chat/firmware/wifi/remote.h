#pragma once
#include "atvv.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <host/ble_hs.h>
#include <nimble/nimble_npl.h>
#include <optional>
struct peer;

namespace wifichat::wifi {
// RC003-MS boundary. NimBLE owns pairing/GATT/capture; UI sends bounded
// commands and takes completed compressed records exactly once after key release.
class Remote {
  public:
    enum class State {
        starting,
        disconnected,
        scanning,
        connecting,
        discovering,
        ready,
        recording,
        failed
    };
    struct Candidate {
        ble_addr_t address{};
        std::array<char, 24> name{};
    };
    struct Snapshot {
        uint32_t revision = 0; // Host increments for every published status/candidate change.
        State state = State::starting;
        std::array<char, 96> message{};
        std::array<Candidate, 4> candidates{};
        size_t count = 0;
    };
    bool initialize();
    Snapshot snapshot();
    bool scan();
    bool pair(const ble_addr_t &address);
    bool disconnect();
    bool forget();
    void allow_recording(bool value) { allowed_.store(value, std::memory_order_release); }
    void cancel_recording();
    // Ownership of ephemeral ATVV records passes to UI; HTTP decodes to 16 kHz PCM.
    bool take_audio(uint8_t *&data, size_t &bytes);

  private:
    enum class Operation { scan, pair, disconnect, forget, cancel };
    enum class Intent { disconnected, connected, forget };
    struct Command {
        Operation operation{};
        ble_addr_t address{};
    };
    bool command(Operation, const ble_addr_t * = nullptr);
    static void host(void *);
    static void synced();
    static void reset(int);
    static void commands(ble_npl_event *);
    static void timer(ble_npl_event *);
    static int gap(ble_gap_event *, void *);
    static int mtu(uint16_t, const ble_gatt_error *, uint16_t, void *);
    static void discovered(const struct peer *, int, void *);
    static int subscribed(uint16_t, const ble_gatt_error *, ble_gatt_attr *, void *);
    void process(const Command &);
    void update(State, const char *);
    void fail(const char *, int code = 0);
    void finish_forget(); // Delete persistent identity only after the link/SMP is gone.
    bool write(const uint8_t *data, size_t bytes);
    void subscribe_next();
    void notification(uint16_t handle, const uint8_t *, size_t);
    void open_microphone();
    void start_audio(uint8_t reason);
    bool close_microphone();
    void finish_audio(atvv::StopReason reason);
    void discard_audio();
    void append_audio(const uint8_t *, size_t);
    static Remote *instance_; // NimBLE's global callbacks bind one boot-lifetime instance.
    QueueHandle_t commands_ = nullptr;
    ble_npl_event event_{};
    ble_npl_callout timer_{};
    portMUX_TYPE mutex_ = portMUX_INITIALIZER_UNLOCKED;
    Snapshot snapshot_{};
    std::atomic<bool> allowed_{false};
    std::atomic<bool> initialized_{false};
    std::atomic<uint32_t> capture_epoch_{1}; // UI cancellation invalidates handoffs synchronously.
    uint32_t recording_epoch_ = 0, completed_epoch_ = 0;
    uint32_t connection_epoch_ = 1; // NimBLE callback tokens; never borrow a mutable context.
    Intent intent_ = Intent::disconnected;
    std::optional<atvv::VoicePress> press_; // Host-owned; closing fences starts until STOP.
    uint8_t own_address_ = 0;
    uint16_t connection_ = BLE_HS_CONN_HANDLE_NONE, command_handle_ = 0, audio_handle_ = 0,
             control_handle_ = 0;
    std::array<uint16_t, 2> subscriptions_{}; // ATVV control/audio CCCDs only.
    size_t subscription_count_ = 0, subscription_index_ = 0;
    ble_addr_t selected_{};
    bool connecting_ = false, capabilities_ = false;
    uint8_t session_ = 0;
    uint32_t decoded_bytes_ = 0;
    uint8_t *buffer_ = nullptr; // NimBLE-owned compressed capture, never shared while recording.
    size_t used_ = 0;
    uint8_t *completed_buffer_ =
        nullptr; // mutex_: immutable handoff, separate from host's recorder.
    size_t completed_bytes_ = 0;
    int64_t started_ = 0, extended_ = 0, setup_deadline_ = 0;
};
} // namespace wifichat::wifi
