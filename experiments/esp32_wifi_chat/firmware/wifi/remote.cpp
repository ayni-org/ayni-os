#include "remote.h"
#include "../main/speaker.h"
#include "capture.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <nvs.h>
extern "C" {
#include <esp_central.h>
#include <host/ble_gap.h>
#include <host/ble_gatt.h>
#include <host/ble_hs_adv.h>
#include <host/util/util.h>
#include <nimble/nimble_port.h>
#include <nimble/nimble_port_freertos.h>
#include <store/config/ble_store_config.h>
void ble_store_config_init(void);
}
namespace wifichat::wifi {
namespace {
constexpr int64_t capture_us = 30000000, extend_us = 2000000, setup_us = 20000000;
constexpr int64_t mic_ack_us = 3000000;
constexpr size_t frame_bytes = 120;
// One selected remote, bounded full GATT database. The initial 40-entry pools
// returned BLE_HS_ENOMEM on the RC003-MS after encryption and MTU negotiation.
constexpr int peer_count = 1, service_capacity = 16, characteristic_capacity = 96,
              descriptor_capacity = 128;
const ble_uuid128_t service = BLE_UUID128_INIT(0x64, 0xb6, 0x17, 0xf6, 0x01, 0xaf, 0x7d, 0xbc, 0x05,
                                               0x4f, 0x21, 0x5a, 0x01, 0x00, 0x5e, 0xab);
const ble_uuid128_t command_uuid = BLE_UUID128_INIT(0x64, 0xb6, 0x17, 0xf6, 0x01, 0xaf, 0x7d, 0xbc,
                                                    0x05, 0x4f, 0x21, 0x5a, 0x02, 0x00, 0x5e, 0xab);
const ble_uuid128_t audio_uuid = BLE_UUID128_INIT(0x64, 0xb6, 0x17, 0xf6, 0x01, 0xaf, 0x7d, 0xbc,
                                                  0x05, 0x4f, 0x21, 0x5a, 0x03, 0x00, 0x5e, 0xab);
const ble_uuid128_t control_uuid = BLE_UUID128_INIT(0x64, 0xb6, 0x17, 0xf6, 0x01, 0xaf, 0x7d, 0xbc,
                                                    0x05, 0x4f, 0x21, 0x5a, 0x04, 0x00, 0x5e, 0xab);
const ble_uuid16_t cccd = BLE_UUID16_INIT(0x2902);
void wipe_free(uint8_t *data, size_t bytes) {
    if (!data) {
        return;
    }
    volatile uint8_t *p = data;
    for (size_t i = 0; i < bytes; ++i) {
        p[i] = 0;
    }
    heap_caps_free(data);
}
} // namespace
Remote *Remote::instance_ = nullptr;
Remote::Snapshot Remote::snapshot() {
    portENTER_CRITICAL(&mutex_);
    auto result = snapshot_;
    portEXIT_CRITICAL(&mutex_);
    return result;
}
void Remote::update(State state, const char *message) {
    portENTER_CRITICAL(&mutex_);
    snapshot_.state = state;
    ++snapshot_.revision;
    std::snprintf(snapshot_.message.data(), snapshot_.message.size(), "%s", message);
    portEXIT_CRITICAL(&mutex_);
    // Fixed device diagnostics only: no addresses, key reports or audio payloads.
    std::printf("remote state=%u %s\n", unsigned(state), message);
}
bool Remote::initialize() {
    if (instance_) {
        return false;
    }
    commands_ = xQueueCreate(8, sizeof(Command));
    if (!commands_) {
        return false;
    }
    if (nimble_port_init() != ESP_OK) {
        vQueueDelete(commands_);
        commands_ = nullptr;
        return false;
    }
    instance_ = this;
    ble_hs_cfg.sync_cb = synced;
    ble_hs_cfg.reset_cb = reset;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_store_config_init();
    if (peer_init(peer_count, service_capacity, characteristic_capacity, descriptor_capacity) !=
        0) {
        instance_ = nullptr;
        nimble_port_deinit();
        vQueueDelete(commands_);
        commands_ = nullptr;
        return false;
    }
    ble_npl_event_init(&event_, commands, this);
    ble_npl_callout_init(&timer_, nimble_port_get_dflt_eventq(), timer, this);
    initialized_.store(true, std::memory_order_release);
    nimble_port_freertos_init(host);
    return true;
}
void Remote::host(void *) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}
void Remote::synced() {
    auto &self = *instance_;
    if (!self.initialized_.load(std::memory_order_acquire)) {
        return;
    }
    if (ble_hs_id_infer_auto(0, &self.own_address_)) {
        self.update(State::failed, "蓝牙地址初始化失败");
        return;
    }
    self.update(State::disconnected, "触屏扫描并选择遥控器");
    ble_addr_t address{};
    size_t size = sizeof(address);
    nvs_handle_t store = 0;
    if (nvs_open("ayni_ble", NVS_READONLY, &store) == ESP_OK) {
        const auto rc = nvs_get_blob(store, "peer", &address, &size);
        nvs_close(store);
        if (rc == ESP_OK && size == sizeof(address)) {
            self.process({Operation::pair, address});
        }
    }
    ble_npl_callout_reset(&self.timer_, ble_npl_time_ms_to_ticks32(100));
}
void Remote::reset(int reason) {
    std::printf("remote host reset=%d\n", reason);
    if (instance_) {
        auto &self = *instance_;
        self.initialized_.store(false, std::memory_order_release);
        self.allowed_.store(false, std::memory_order_release);
        self.capture_epoch_.fetch_add(1, std::memory_order_acq_rel);
        ++self.connection_epoch_;
        self.intent_ = Intent::disconnected;
        self.setup_deadline_ = 0;
        self.discard_audio();
        self.press_.reset();
        self.update(State::failed, "蓝牙已复位，请重启设备");
    }
}
bool Remote::command(Operation operation, const ble_addr_t *address) {
    if (!initialized_.load(std::memory_order_acquire)) {
        return false;
    }
    Command command{};
    command.operation = operation;
    if (address) {
        command.address = *address;
    }
    if (xQueueSend(commands_, &command, 0) != pdTRUE) {
        return false;
    }
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &event_);
    return true;
}
bool Remote::scan() {
    return command(Operation::scan);
}
bool Remote::pair(const ble_addr_t &address) {
    return command(Operation::pair, &address);
}
bool Remote::disconnect() {
    allowed_.store(false, std::memory_order_release);
    capture_epoch_.fetch_add(1, std::memory_order_acq_rel);
    return command(Operation::disconnect);
}
bool Remote::forget() {
    allowed_.store(false, std::memory_order_release);
    capture_epoch_.fetch_add(1, std::memory_order_acq_rel);
    return command(Operation::forget);
}
void Remote::cancel_recording() {
    allowed_.store(false);
    capture_epoch_.fetch_add(1, std::memory_order_acq_rel);
    command(Operation::cancel);
}
void Remote::commands(ble_npl_event *event) {
    auto &self = *static_cast<Remote *>(ble_npl_event_get_arg(event));
    Command command{};
    while (xQueueReceive(self.commands_, &command, 0) == pdTRUE) {
        self.process(command);
    }
}
void Remote::process(const Command &command) {
    if (!initialized_.load(std::memory_order_acquire)) {
        return;
    }
    if (command.operation == Operation::cancel) {
        // A late host event only cancels the old generation, never a later press.
        const auto current = capture_epoch_.load(std::memory_order_acquire);
        if (press_ && press_->epoch != current) {
            close_microphone();
        }
        if (buffer_ && recording_epoch_ != current) {
            wipe_free(buffer_, used_);
            buffer_ = nullptr;
            used_ = 0;
            started_ = 0;
        }
        uint8_t *old = nullptr;
        size_t bytes = 0;
        portENTER_CRITICAL(&mutex_);
        if (completed_buffer_ && completed_epoch_ != current) {
            old = completed_buffer_;
            bytes = completed_bytes_;
            completed_buffer_ = nullptr;
            completed_bytes_ = 0;
        }
        portEXIT_CRITICAL(&mutex_);
        wipe_free(old, bytes);
        if (capabilities_ && !buffer_) {
            update(State::ready, "录音已取消");
        }
        return;
    }
    if (command.operation == Operation::disconnect || command.operation == Operation::forget) {
        // Preserve a pending forget until the asynchronous SMP/link teardown completes.
        if (command.operation == Operation::forget) {
            intent_ = Intent::forget;
        } else if (intent_ != Intent::forget) {
            intent_ = Intent::disconnected;
        }
        ++connection_epoch_;
        capabilities_ = false;
        setup_deadline_ = 0;
        ble_gap_disc_cancel();
        discard_audio();
        press_.reset();
        const auto rc = connecting_ ? ble_gap_conn_cancel()
                        : connection_ != BLE_HS_CONN_HANDLE_NONE
                            ? ble_gap_terminate(connection_, BLE_ERR_REM_USER_CONN_TERM)
                            : 0;
        if (rc) {
            update(State::failed, "断开未获确认，请重试或重启");
        } else if (connecting_ || connection_ != BLE_HS_CONN_HANDLE_NONE) {
            update(State::disconnected, "正在断开遥控器，请稍候");
        } else if (intent_ == Intent::forget) {
            finish_forget();
        } else {
            update(State::disconnected, "遥控器已断开");
        }
        return;
    }
    if (intent_ == Intent::forget || connection_ != BLE_HS_CONN_HANDLE_NONE || connecting_) {
        // Keep the actual handshake failure visible instead of overwriting it.
        const auto current = snapshot();
        std::printf("remote scan refused state=%u connected=%u connecting=%u\n",
                    unsigned(current.state), unsigned(connection_ != BLE_HS_CONN_HANDLE_NONE),
                    unsigned(connecting_));
        return;
    }
    if (command.operation == Operation::scan) {
        portENTER_CRITICAL(&mutex_);
        snapshot_.count = 0;
        ++snapshot_.revision;
        portEXIT_CRITICAL(&mutex_);
        ble_gap_disc_params params{};
        params.itvl = 160;
        params.window = 80;
        params.passive = 0;
        params.filter_duplicates = 0;
        const auto rc = ble_gap_disc(own_address_, 20000, &params, gap, this);
        update(rc ? State::failed : State::scanning,
               rc ? "蓝牙扫描启动失败" : "请将遥控器置于配对模式");
    } else {
        ble_gap_disc_cancel();
        intent_ = Intent::connected;
        ++connection_epoch_;
        selected_ = command.address;
        connecting_ = true;
        const auto rc = ble_gap_connect(own_address_, &selected_, 15000, nullptr, gap, this);
        if (rc) {
            connecting_ = false;
        }
        setup_deadline_ = esp_timer_get_time() + setup_us;
        update(rc ? State::failed : State::connecting, rc ? "连接启动失败" : "正在连接遥控器");
    }
}
void Remote::finish_forget() {
    ble_addr_t addresses[2]{};
    int count = 0;
    bool ok = ble_store_util_bonded_peers(addresses, &count, 2) == 0;
    if (ok) {
        for (int i = 0; i < count; ++i) {
            ok = ble_store_util_delete_peer(&addresses[i]) == 0 && ok;
        }
    }
    nvs_handle_t store = 0;
    if (nvs_open("ayni_ble", NVS_READWRITE, &store) == ESP_OK) {
        const auto erased = nvs_erase_key(store, "peer");
        ok = (erased == ESP_OK || erased == ESP_ERR_NVS_NOT_FOUND) && nvs_commit(store) == ESP_OK &&
             ok;
        nvs_close(store);
    } else {
        ok = false;
    }
    // A failed deletion remains fenced from new connections until explicitly retried.
    intent_ = ok ? Intent::disconnected : Intent::forget;
    update(ok ? State::disconnected : State::failed,
           ok ? "配对已清除" : "配对删除未获确认，请重试");
}
void Remote::fail(const char *message, int code) {
    intent_ = Intent::disconnected;
    ++connection_epoch_;
    setup_deadline_ = 0;
    discard_audio();
    press_.reset();
    capabilities_ = false;
    std::array<char, 96> detail{};
    if (code) {
        std::snprintf(detail.data(), detail.size(), "%s [%d]", message, code);
    } else {
        std::snprintf(detail.data(), detail.size(), "%s", message);
    }
    update(State::failed, detail.data());
    if (connecting_) {
        ble_gap_conn_cancel();
    }
    if (connection_ != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(connection_, BLE_ERR_REM_USER_CONN_TERM);
    }
}
bool Remote::write(const uint8_t *data, size_t bytes) {
    return connection_ != BLE_HS_CONN_HANDLE_NONE && command_handle_ &&
           ble_gattc_write_no_rsp_flat(connection_, command_handle_, data, bytes) == 0;
}
int Remote::gap(ble_gap_event *event, void *context) {
    auto &self = *static_cast<Remote *>(context);
    if (!self.initialized_.load(std::memory_order_acquire)) {
        return 0;
    }
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        ble_hs_adv_fields fields{};
        if (ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data) ||
            !fields.name || fields.name_len < 5 || std::memcmp(fields.name, "MI RC", 5)) {
            break;
        }
        portENTER_CRITICAL(&self.mutex_);
        bool duplicate = false;
        for (size_t i = 0; i < self.snapshot_.count; ++i) {
            if (!std::memcmp(&self.snapshot_.candidates[i].address, &event->disc.addr,
                             sizeof(ble_addr_t))) {
                duplicate = true;
            }
        }
        if (!duplicate && self.snapshot_.count < self.snapshot_.candidates.size()) {
            auto &candidate = self.snapshot_.candidates[self.snapshot_.count++];
            ++self.snapshot_.revision;
            candidate.address = event->disc.addr;
            const auto count = std::min(size_t(fields.name_len), candidate.name.size() - 1);
            std::memcpy(candidate.name.data(), fields.name, count);
            candidate.name[count] = 0;
        }
        portEXIT_CRITICAL(&self.mutex_);
        break;
    }
    case BLE_GAP_EVENT_DISC_COMPLETE:
        if (self.snapshot().state == State::scanning) {
            self.update(State::disconnected, "扫描结束，点击候选项连接");
        }
        break;
    case BLE_GAP_EVENT_CONNECT: {
        self.setup_deadline_ = 0;
        self.connecting_ = false;
        if (self.intent_ != Intent::connected) {
            if (!event->connect.status) {
                self.connection_ = event->connect.conn_handle;
                if (ble_gap_terminate(self.connection_, BLE_ERR_REM_USER_CONN_TERM)) {
                    self.update(State::failed, "撤销连接未获确认，请重启");
                }
            } else if (self.intent_ == Intent::forget) {
                self.finish_forget();
            } else {
                self.update(State::disconnected, "遥控器连接已取消");
            }
            break;
        }
        if (event->connect.status) {
            std::printf("remote connect status=%d\n", event->connect.status);
            self.intent_ = Intent::disconnected;
            self.update(State::disconnected, "连接未成功，请重新选择");
            break;
        }
        self.connection_ = event->connect.conn_handle;
        self.setup_deadline_ = esp_timer_get_time() + setup_us;
        self.update(State::discovering, "正在加密遥控器");
        auto rc = peer_add(self.connection_);
        if (!rc) {
            rc = ble_gap_security_initiate(self.connection_);
        }
        if (rc) {
            self.fail("遥控器加密启动失败", rc);
        }
        break;
    }
    case BLE_GAP_EVENT_ENC_CHANGE: {
        if (self.intent_ != Intent::connected ||
            event->enc_change.conn_handle != self.connection_) {
            break;
        }
        ble_gap_conn_desc description{};
        const auto found = ble_gap_conn_find(self.connection_, &description);
        std::printf("remote encryption status=%d lookup=%d encrypted=%u\n",
                    event->enc_change.status, found, unsigned(description.sec_state.encrypted));
        if (event->enc_change.status || found || !description.sec_state.encrypted) {
            self.fail("加密失败，请清除配对后重试",
                      event->enc_change.status ? event->enc_change.status : found);
            break;
        }
        self.update(State::discovering, "正在协商蓝牙 MTU");
        const auto rc = ble_gattc_exchange_mtu(
            self.connection_, mtu, reinterpret_cast<void *>(uintptr_t(self.connection_epoch_)));
        if (rc) {
            self.fail("协商蓝牙 MTU 失败", rc);
        }
        break;
    }
    case BLE_GAP_EVENT_L2CAP_UPDATE_REQ:
    case BLE_GAP_EVENT_CONN_UPDATE_REQ:
        if (event->conn_update_req.peer_params && event->conn_update_req.peer_params->latency) {
            // Long skipped-event intervals turn GATT setup into a multi-second
            // exchange per attribute and exceed the bounded voice setup deadline.
            std::printf("remote rejected slave latency=%u\n",
                        unsigned(event->conn_update_req.peer_params->latency));
            return BLE_ERR_CONN_PARMS;
        }
        break;
    case BLE_GAP_EVENT_CONN_UPDATE: {
        ble_gap_conn_desc connection{};
        if (!ble_gap_conn_find(self.connection_, &connection)) {
            std::printf("remote parameters status=%d interval=%u latency=%u\n",
                        event->conn_update.status, unsigned(connection.conn_itvl),
                        unsigned(connection.conn_latency));
        }
        break;
    }
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        self.fail("配对密钥已变化，请在触屏清除配对");
        return BLE_GAP_REPEAT_PAIRING_IGNORE;
    }
    case BLE_GAP_EVENT_NOTIFY_RX: {
        if (self.intent_ != Intent::connected || event->notify_rx.conn_handle != self.connection_) {
            break;
        }
        uint8_t bytes[247]{};
        const auto count = OS_MBUF_PKTLEN(event->notify_rx.om);
        if (count <= sizeof(bytes) && !os_mbuf_copydata(event->notify_rx.om, 0, count, bytes)) {
            self.notification(event->notify_rx.attr_handle, bytes, count);
        } else if (event->notify_rx.attr_handle == self.audio_handle_) {
            self.fail("语音帧超限，整段录音已丢弃");
        }
        break;
    }
    case BLE_GAP_EVENT_DISCONNECT: {
        if (event->disconnect.conn.conn_handle != self.connection_) {
            break;
        }
        const auto previous = self.snapshot();
        std::printf("remote disconnected reason=%d\n", event->disconnect.reason);
        ++self.connection_epoch_;
        self.capture_epoch_.fetch_add(1, std::memory_order_acq_rel);
        peer_delete(event->disconnect.conn.conn_handle);
        self.connection_ = BLE_HS_CONN_HANDLE_NONE;
        self.command_handle_ = self.audio_handle_ = self.control_handle_ = 0;
        self.capabilities_ = false;
        self.setup_deadline_ = 0;
        self.discard_audio();
        self.press_.reset();
        if (self.intent_ == Intent::forget) {
            self.finish_forget();
        } else {
            self.intent_ = Intent::disconnected;
            if (previous.state != State::failed) {
                self.update(State::disconnected, "遥控器已断开，点击扫描重连");
            }
        }
        break;
    }
    default:
        break;
    }
    return 0;
}
int Remote::mtu(uint16_t connection, const ble_gatt_error *error, uint16_t value, void *context) {
    auto &self = *instance_;
    if (self.intent_ != Intent::connected || uintptr_t(context) != self.connection_epoch_ ||
        connection != self.connection_) {
        return 0;
    }
    std::printf("remote mtu status=%d size=%u\n", error->status, unsigned(value));
    if (error->status || value < frame_bytes + 3) {
        self.fail("蓝牙 MTU 不足或协商失败", error->status);
        return 0;
    }
    self.update(State::discovering, "正在发现 GATT 服务");
    const auto rc = peer_disc_all(connection, discovered, context);
    if (rc) {
        self.fail("GATT 服务发现启动失败", rc);
    }
    return 0;
}
void Remote::discovered(const peer *peer, int status, void *context) {
    auto &self = *instance_;
    if (self.intent_ != Intent::connected || uintptr_t(context) != self.connection_epoch_) {
        return;
    }
    size_t services = 0, characteristics = 0, descriptors = 0;
    if (peer) {
        const peer_svc *svc = nullptr;
        SLIST_FOREACH(svc, &peer->svcs, next) {
            ++services;
            const peer_chr *chr = nullptr;
            SLIST_FOREACH(chr, &svc->chrs, next) {
                ++characteristics;
                const peer_dsc *dsc = nullptr;
                SLIST_FOREACH(dsc, &chr->dscs, next) {
                    ++descriptors;
                }
            }
        }
    }
    std::printf("remote discovery status=%d services=%zu characteristics=%zu descriptors=%zu\n",
                status, services, characteristics, descriptors);
    if (status || !peer) {
        self.fail(status == BLE_HS_ENOMEM ? "遥控器服务发现容量不足" : "发现遥控器服务失败",
                  status);
        return;
    }
    const auto *command = peer_chr_find_uuid(peer, &service.u, &command_uuid.u);
    const auto *audio = peer_chr_find_uuid(peer, &service.u, &audio_uuid.u);
    const auto *control = peer_chr_find_uuid(peer, &service.u, &control_uuid.u);
    if (!command || !audio || !control ||
        !(command->chr.properties & BLE_GATT_CHR_PROP_WRITE_NO_RSP)) {
        self.fail("遥控器缺少受支持的 ATVV 服务");
        return;
    }
    self.command_handle_ = command->chr.val_handle;
    self.audio_handle_ = audio->chr.val_handle;
    self.control_handle_ = control->chr.val_handle;
    self.subscription_count_ = self.subscription_index_ = 0;
    const auto add = [&](const peer_chr *characteristic) {
        if (!(characteristic->chr.properties & BLE_GATT_CHR_PROP_NOTIFY) ||
            self.subscription_count_ >= self.subscriptions_.size()) {
            return false;
        }
        const peer_dsc *descriptor = nullptr;
        SLIST_FOREACH(descriptor, &characteristic->dscs, next) {
            if (!ble_uuid_cmp(&descriptor->dsc.uuid.u, &cccd.u)) {
                self.subscriptions_[self.subscription_count_++] = descriptor->dsc.handle;
                return true;
            }
        }
        return false;
    };
    if (!add(control) || !add(audio)) {
        self.fail("语音通知订阅不可用");
        return;
    }
    self.update(State::discovering, "正在订阅遥控器通知");
    self.subscribe_next();
}
void Remote::subscribe_next() {
    if (subscription_index_ < subscription_count_) {
        const uint8_t enable[] = {1, 0};
        if (ble_gattc_write_flat(connection_, subscriptions_[subscription_index_], enable,
                                 sizeof(enable), subscribed,
                                 reinterpret_cast<void *>(uintptr_t(connection_epoch_)))) {
            fail("订阅遥控器通知失败");
        }
    } else {
        update(State::discovering, "正在等待语音格式协商");
        const uint8_t capabilities[] = {0x0a, 0x01, 0, 0, 0x03, 0x03};
        if (!write(capabilities, sizeof(capabilities))) {
            fail("语音格式协商失败");
        }
    }
}
int Remote::subscribed(uint16_t connection, const ble_gatt_error *error, ble_gatt_attr *,
                       void *context) {
    auto &self = *instance_;
    if (self.intent_ != Intent::connected || uintptr_t(context) != self.connection_epoch_ ||
        connection != self.connection_) {
        return 0;
    }
    if (error->status) {
        self.fail("遥控器通知未获确认", error->status);
    } else {
        ++self.subscription_index_;
        self.subscribe_next();
    }
    return 0;
}
void Remote::open_microphone() {
    const auto epoch = capture_epoch_.load(std::memory_order_acquire);
    if (press_ || !capabilities_ || !allowed_.load(std::memory_order_acquire) ||
        epoch != capture_epoch_.load(std::memory_order_acquire)) {
        return;
    }
    press_ = atvv::VoicePress{atvv::MicPhase::opening, epoch, esp_timer_get_time() + mic_ack_us};
    const uint8_t open[] = {atvv::mic_open, 0};
    if (!write(open, sizeof(open))) {
        fail("启动遥控器麦克风失败");
    }
}
bool Remote::close_microphone() {
    const auto epoch = press_ ? press_->epoch : capture_epoch_.load(std::memory_order_acquire);
    const auto deadline = press_ && press_->phase == atvv::MicPhase::closing
                              ? press_->deadline
                              : esp_timer_get_time() + mic_ack_us;
    const auto command = atvv::close_command(session_);
    if (!write(command.data(), command.size())) {
        fail("关闭遥控器麦克风失败");
        return false;
    }
    press_ = atvv::VoicePress{atvv::MicPhase::closing, epoch, deadline};
    return true;
}
void Remote::start_audio(uint8_t reason) {
    // Snapshot generation before admission, including the autonomous HTT path.
    const auto current = capture_epoch_.load(std::memory_order_acquire);
    const auto epoch =
        atvv::start_epoch(press_, current, allowed_.load(std::memory_order_acquire), reason);
    std::printf("remote audio start reason=%u admitted=%u pending_press=%u\n", unsigned(reason),
                unsigned(epoch.has_value()), unsigned(press_.has_value()));
    portENTER_CRITICAL(&mutex_);
    const bool pending = completed_buffer_ != nullptr;
    portEXIT_CRITICAL(&mutex_);
    if (!epoch || current != capture_epoch_.load(std::memory_order_acquire) || !capabilities_ ||
        buffer_ || pending) {
        if (close_microphone()) {
            update(State::ready, "本次语音未受理，请查看主界面会话状态");
        }
        return;
    }
    if (!press_) {
        press_ = atvv::VoicePress{atvv::MicPhase::opening, *epoch, 0};
    }
    buffer_ = static_cast<uint8_t *>(
        heap_caps_malloc(capture::capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!buffer_) {
        if (!close_microphone()) {
            return;
        }
        update(State::ready, "录音内存不足");
        return;
    }
    if (*epoch != capture_epoch_.load(std::memory_order_acquire) ||
        !allowed_.load(std::memory_order_acquire)) {
        heap_caps_free(buffer_);
        buffer_ = nullptr;
        close_microphone();
        return;
    }
    press_->phase = atvv::MicPhase::recording;
    press_->deadline = 0;
    recording_epoch_ = *epoch;
    used_ = capture::header_bytes;
    decoded_bytes_ = 0;
    // RC003 v1.0 starts its IMA predictor/index at zero. AUDIO_SYNC may arrive
    // after audio, so record that initial state and retain every early frame.
    const uint8_t initial[capture::sync_bytes]{};
    if (!capture::append(buffer_, capture::capacity, used_, capture::sync, initial,
                         sizeof(initial))) {
        fail("录音初始状态保存失败");
        return;
    }
    started_ = extended_ = esp_timer_get_time();
    update(State::recording, "正在录音，松键后自动发送");
}
void Remote::discard_audio() {
    wipe_free(buffer_, used_);
    buffer_ = nullptr;
    used_ = 0;
    portENTER_CRITICAL(&mutex_);
    auto *data = completed_buffer_;
    const auto bytes = completed_bytes_;
    completed_buffer_ = nullptr;
    completed_bytes_ = 0;
    portEXIT_CRITICAL(&mutex_);
    wipe_free(data, bytes);
    started_ = 0;
}
void Remote::finish_audio(atvv::StopReason reason) {
    // Both normal stop reasons are terminal remote facts. A MIC_CLOSE sent
    // after HTT release may be ignored (ATVV v1.0 4.7.3), so never await it here.
    press_.reset();
    if (snapshot().state != State::recording) {
        return;
    }
    std::printf("remote recording ended reason=%u pcm_bytes=%lu\n", unsigned(reason),
                static_cast<unsigned long>(decoded_bytes_));
    if (!decoded_bytes_ || recording_epoch_ != capture_epoch_.load(std::memory_order_acquire)) {
        discard_audio();
        update(State::ready, "录音为空或已取消，未发送");
        return;
    }
    capture::finish(buffer_, used_, decoded_bytes_);
    portENTER_CRITICAL(&mutex_);
    completed_buffer_ = buffer_;
    completed_bytes_ = used_;
    completed_epoch_ = recording_epoch_;
    portEXIT_CRITICAL(&mutex_);
    buffer_ = nullptr;
    used_ = 0;
    started_ = 0;
    update(State::ready, "录音完成，正在转写");
}
bool Remote::take_audio(uint8_t *&data, size_t &bytes) {
    portENTER_CRITICAL(&mutex_);
    const bool available = completed_buffer_ != nullptr;
    const bool ready =
        available && completed_epoch_ == capture_epoch_.load(std::memory_order_acquire);
    if (available) {
        data = completed_buffer_;
        bytes = completed_bytes_;
        completed_buffer_ = nullptr;
        completed_bytes_ = 0;
    }
    portEXIT_CRITICAL(&mutex_);
    if (available && !ready) {
        wipe_free(data, bytes);
        data = nullptr;
        bytes = 0;
    }
    return ready;
}
void Remote::append_audio(const uint8_t *data, size_t bytes) {
    if (snapshot().state != State::recording || !buffer_) {
        return;
    }
    if (!decoded_bytes_) {
        std::printf("remote first audio notification=%zu bytes\n", bytes);
    }
    const auto frames = bytes / capture::frame_bytes;
    const auto pcm_bytes = frames * capture::pcm_bytes_per_frame;
    bool valid = bytes && bytes % capture::frame_bytes == 0 &&
                 pcm_bytes <= Speaker::maximum_pcm_bytes - decoded_bytes_;
    // An ATT notification may coalesce complete frames; record each codec frame.
    for (size_t frame = 0; valid && frame < frames; ++frame) {
        valid = capture::append(buffer_, capture::capacity, used_, capture::audio,
                                data + frame * capture::frame_bytes, capture::frame_bytes);
    }
    if (!valid) {
        if (!close_microphone()) {
            return;
        }
        discard_audio();
        update(State::ready, "语音帧无效或录音超限，整段未发送");
        return;
    }
    decoded_bytes_ += uint32_t(pcm_bytes);
}
void Remote::notification(uint16_t handle, const uint8_t *data, size_t bytes) {
    if (!bytes) {
        return;
    }
    if (handle == audio_handle_) {
        append_audio(data, bytes);
        return;
    }
    if (handle == control_handle_) {
        if (data[0] != 0x0a) {
            std::printf("remote control op=%u bytes=%zu argument=%u stream=%u\n", unsigned(data[0]),
                        bytes, unsigned(bytes >= 2 ? data[1] : 0), unsigned(session_));
        }
        if (data[0] == 0x0b && bytes >= 7) {
            if (((unsigned(data[5]) << 8U) | data[6]) != frame_bytes) {
                fail("不支持此遥控器语音格式");
                return;
            }
            ble_gap_conn_desc connection{};
            nvs_handle_t store = 0;
            if (ble_gap_conn_find(connection_, &connection) ||
                nvs_open("ayni_ble", NVS_READWRITE, &store) != ESP_OK) {
                fail("保存遥控器身份失败");
                return;
            }
            auto saved = nvs_set_blob(store, "peer", &connection.peer_id_addr,
                                      sizeof(connection.peer_id_addr));
            if (saved == ESP_OK) {
                saved = nvs_commit(store);
            }
            nvs_close(store);
            if (saved != ESP_OK) {
                fail("遥控器身份未持久保存");
                return;
            }
            capabilities_ = true;
            setup_deadline_ = 0;
            update(State::ready, "遥控器就绪，按住语音键说话");
        } else if (data[0] == atvv::audio_start && bytes >= 4) {
            if (data[2] != atvv::adpcm_16khz) {
                fail("遥控器不是 16 kHz ADPCM 语音");
                return;
            }
            const auto session = bytes >= 4 ? data[3] : 0;
            if (snapshot().state == State::recording) {
                if (session == session_) {
                    return; // Duplicate START is not a second capture.
                }
                if (!close_microphone()) {
                    return;
                }
                discard_audio();
                update(State::ready, "语音会话变化，旧录音未发送");
                return;
            }
            session_ = session;
            start_audio(data[1]);
        } else if (data[0] == atvv::audio_stop) {
            const auto reason = atvv::stop_reason(data, bytes);
            if (reason == atvv::StopReason::closed || reason == atvv::StopReason::released) {
                finish_audio(*reason);
            } else if (reason) {
                discard_audio();
                press_.reset();
                update(State::ready, "语音流被中断，本段未发送");
            }
        } else if (data[0] == atvv::start_search) {
            if (press_ && press_->phase == atvv::MicPhase::recording) {
                close_microphone();
            } else {
                open_microphone();
            }
        } else if (data[0] == 0x0a && bytes >= 7 && snapshot().state == State::recording) {
            if (data[1] != atvv::adpcm_16khz) {
                fail("录音编码变化，仅支持 16 kHz ADPCM");
                return;
            }
            if (data[6] > 88) {
                fail("语音同步参数无效");
                return;
            }
            const uint8_t sync[] = {data[5], data[4], data[6]};
            if (!buffer_ || !capture::append(buffer_, capture::capacity, used_, capture::sync, sync,
                                             sizeof(sync))) {
                fail("语音同步缓冲超限");
                return;
            }
        }
        return;
    }
    // Voice uses ATVV only. HOGP key reports are deliberately not subscribed:
    // their late duplicate DOWN must never reopen an already cancelled stream.
}
void Remote::timer(ble_npl_event *event) {
    auto &self = *static_cast<Remote *>(ble_npl_event_get_arg(event));
    if (!self.initialized_.load(std::memory_order_acquire)) {
        return;
    }
    const auto now = esp_timer_get_time();
    if (self.setup_deadline_ && now >= self.setup_deadline_ && !self.capabilities_) {
        self.setup_deadline_ = 0;
        const auto phase = self.snapshot();
        std::array<char, 96> message{};
        constexpr char prefix[] = "握手超时：";
        std::snprintf(message.data(), message.size(), "%s%.*s", prefix,
                      int(message.size() - sizeof(prefix)), phase.message.data());
        self.fail(message.data());
    }
    if (self.press_ && self.press_->deadline && now >= self.press_->deadline) {
        self.fail("遥控器麦克风确认超时，请重新连接");
    }
    if (self.snapshot().state == State::recording) {
        if (now - self.started_ >= capture_us) {
            if (self.close_microphone()) {
                self.discard_audio();
                self.update(State::ready, "录音超过 30 秒，整段未发送");
            }
        } else if (now - self.extended_ >= extend_us) {
            const uint8_t extend[] = {0x0e, self.session_};
            if (!self.write(extend, sizeof(extend))) {
                self.fail("录音保活失败，未发送");
            }
            self.extended_ = now;
        }
    }
    ble_npl_callout_reset(&self.timer_, ble_npl_time_ms_to_ticks32(100));
}
} // namespace wifichat::wifi
