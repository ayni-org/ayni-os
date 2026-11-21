#include "runtime.h"
#include "../main/speaker.h"
#include "../main/turn_diagnostics.h"
#include "../main/turn_status.h"
#include "../main/wake.h"
#include "api.h"
#include "app_menu.h"
#include "canvas.h"
#include "motor_panel.h"
#include "network_time.h"
#include "ota_update.h"
#include "preferences.h"
#include "remote.h"
#include "reset_worker.h"
#include "station.h"
#include "touch.h"
#include "web_config.h"
#include <algorithm>
#include <ayni/model_messages.hpp>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <new>
#include <optional>
#include <psa/crypto.h>
#include <string>
#include <yyjson.h>
namespace wifichat::wifi {
namespace {
constexpr int64_t redraw_us = 300000;
constexpr size_t recent_stages = 3;
constexpr size_t network_fields[]{0, 1}, model_fields[]{3, 4, 5, 2};
constexpr int remote_candidates_top = 90, remote_candidate_pitch = 46;
constexpr const char *labels[] = {"Wi-Fi 网络", "密码", "语音 API", "模型 API", "API 密钥",
                                  "模型名称",   "接口", "流式预览", "输出字段"};
std::string_view keyboard_keys(bool symbols) {
    return symbols ? "!@#$%^&*()_+-=[]{}:<>?~`|\\\"' ABCDEFGHIJK"
                   : "qwertyuiopasdfghjkl;zxcvbnm,./0123456789";
}
void resources(DiagnosticLog &log, const char *stage) {
    log.record(
        "wifi resources %s internal_free=%zu psram_free=%zu psram_largest=%zu io_stack_free=%lu\n",
        stage, heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
        static_cast<unsigned long>(uxTaskGetStackHighWaterMark(nullptr)));
}
enum class Page {
    launcher,
    voice,
    conversation,
    history,
    status,
    settings,
    keyboard,
    remote,
    motor,
    networks,
    confirm_clear,
    maintenance,
    confirm_forget,
    confirm_peer
};
class Runtime {
  public:
    explicit Runtime(Transport &transport) : transport_(transport) {}
    void run();

  private:
    enum class Restart { none, settings, rebuild, ota };
    bool restarting() const { return restart_ != Restart::none; }
    void packets();
    void stage(std::string_view text);
    void tool_outline(std::string_view body);
    std::string stages(size_t maximum) const;
    void results();
    void tap(int x, int y);
    void tap_confirmation(int x, int y);
    void tap_remote(int x, int y);
    void tap_networks(int x, int y);
    void tap_keyboard(int x, int y);
    void tap_settings(int x, int y);
    void tap_home(int x, int y);
    void draw();
    void draw_status();
    void draw_confirmation();
    void draw_remote();
    void draw_networks();
    void draw_keyboard();
    void draw_settings();
    void draw_home();
    void open_app(App);
    void toggle_voice();
    void configure();
    void clear_preview();
    void cancel();
    void start_motor_check(uint32_t motor);
    void send(std::string_view text, uint64_t id = 0);
    void save_restart();
    void web_changes();
    void begin_restart();
    void begin_rebuild();
    void stop_for_restart();
    void reset_progress();
    void update_progress();
    void draw_maintenance();
    void message(std::string_view text) {
        if (status_ == text) {
            return;
        }
        status_.assign(text);
        dirty_ = true;
    }
    Transport &transport_;
    Settings settings_{};
    Preferences preferences_;
    bool preferences_ok_ = false;
    struct VoiceSetting {
        uint64_t id;
        bool enabled;
        bool queued;
    };
    std::optional<VoiceSetting> voice_setting_; // Exact owner acknowledgement gates new input.
    Canvas canvas_{};
    Touch touch_{};
    Station station_{};
    Remote remote_{};
    MotorPanel motor_{};
    Api api_{};
    UiFrameWindow last_frame_{}; // chat-io only; monotonic draw()/paint() window of last frame.
    WebConfig web_{};
    OtaUpdate update_{};
    bool web_started_ = false;
    Packet packet_{};
    bool storage_ok_ = false, touch_ok_ = false, configured_ = false;
    enum class Session { unconfigured, opening, ready, failed, closing, stopped };
    Session session_ = Session::unconfigured;
    bool ready() const { return session_ == Session::ready && !update_.pending_verification(); }
    bool model_clock_ready() const {
        return request_clock_ready(settings_.model_url.data(), std::time(nullptr));
    }
    void session_status(Session state, std::string_view detail) {
        session_ = state;
        session_detail_.assign(detail);
        message(detail);
        // This is boot/session metadata, never model output or a transcript.
        transport_.diagnostics().record("wifi session=%u %.*s\n", unsigned(state),
                                        int(detail.size()), detail.data());
    }
    enum class Work { idle, recognizing, running, cancelling };
    Work work_ = Work::idle; // A new user turn cannot overlap recognition or cancellation.
    std::optional<uint64_t> motor_check_id_; // Present until the reliable check terminal arrives.
    bool busy() const {
        return capacity_ != Capacity::idle || motor_check_id_.has_value() ||
               work_ == Work::running || work_ == Work::cancelling;
    }
    bool recognizing() const { return work_ == Work::recognizing; }
    bool api_started_ = false, api_failed_ = false, dirty_ = true;
    enum class Capacity { idle, template_turn, loop_turn, measuring };
    Capacity capacity_ = Capacity::idle; // UI-owned synthetic turn, then API scratch test.
    Restart restart_ = Restart::none;
    ResetWorker reset_;
    ModelSelection selection_{};
    std::array<char, 17> route_{}; // Frozen with Api's active settings, separate from the UI draft.
    bool shifted_ = false, symbols_ = false, was_connected_ = false;
    bool was_clock_ready_ = false; // UI redraw tracking only; does not own/copy system time.
    const char *voice_hint_ = "重启后保留";
    enum class SettingsView { network, model, options };
    SettingsView settings_view_ = SettingsView::network;
    Page page_ = Page::launcher;
    size_t field_ = 0, current_page_ = 0, pages_ = 1, peer_index_ = 0;
    uint64_t document_turn_ = 0; // Last turn that explicitly used the screen capability.
    uint64_t turn_ = 0, next_id_ = uint64_t(esp_random()) << 32U;
    static constexpr size_t stage_capacity = 8;
    std::array<std::string, stage_capacity> stages_{};
    size_t stage_count_ = 0;
    uint32_t model_round_ = 0;
    uint64_t active_model_ = 0;
    Api::ModelPreview preview_packet_{};
    std::string preview_text_;
    std::string document_, transcript_, answer_, status_ = "请先在触屏设置 Wi-Fi 和 API";
    std::string speech_error_; // Fixed backend diagnostic only, cleared for a new turn.
    std::string session_detail_ = "等待 Wi-Fi 和模型配置";
    std::array<char, 192> editing_{};
    std::array<wifi_ap_record_t, 8> networks_{};
    size_t network_count_ = 0;
    Remote::Snapshot remote_snapshot_{};
    int64_t restart_at_ = 0;
};
void Runtime::stage(std::string_view text) {
    if (work_ != Work::running) {
        return;
    } // Late progress never reopens cancellation or idle.
    if (!stage_count_ || stages_[stage_count_ - 1] != text) {
        if (stage_count_ == stage_capacity) {
            for (size_t at = 1; at < stage_capacity; ++at) {
                stages_[at - 1] = std::move(stages_[at]);
            }
            --stage_count_;
        }
        stages_[stage_count_++].assign(text);
    }
    if (page_ == Page::history) {
        current_page_ = 0;
    }
    message(text);
}
std::string Runtime::stages(size_t maximum) const {
    std::string result;
    const auto first = stage_count_ > maximum ? stage_count_ - maximum : 0;
    for (size_t at = first; at < stage_count_; ++at) {
        result += stages_[at];
        result += '\n';
    }
    return result;
}
void Runtime::tool_outline(std::string_view body) {
    if (body.size() > tool_outline_bytes) {
        return;
    }
    auto *doc = yyjson_read(body.data(), body.size(), 0);
    if (!doc) {
        return;
    }
    auto *root = yyjson_doc_get_root(doc);
    if (yyjson_is_arr(root) && yyjson_arr_size(root) <= AYNI_CALL_TOOL_COUNT_MAX) {
        size_t at, count;
        yyjson_val *item;
        yyjson_arr_foreach(root, at, count, item) {
            auto *name = yyjson_obj_get(item, "function"),
                 *binding = yyjson_obj_get(item, "binding");
            if (!yyjson_is_str(name)) {
                continue;
            }
            const std::string_view function(yyjson_get_str(name), yyjson_get_len(name));
            std::string label = "模型提议：";
            if (function == ayni::model::list_function) {
                label += "发现可用能力";
            } else if (function == ayni::model::inspect_function) {
                label += "查看能力说明";
            } else if (function == ayni::model::call_function) {
                label += "调用能力";
                auto *operation = yyjson_obj_get(item, "operation");
                if (yyjson_is_str(operation) && yyjson_get_len(operation) <= 6) {
                    label += ' ';
                    label.append(yyjson_get_str(operation), yyjson_get_len(operation));
                }
            } else {
                label += "使用工具";
            }
            if (yyjson_is_str(binding) && yyjson_get_len(binding) <= AYNI_CATALOG_ID_MAX_BYTES &&
                yyjson_get_len(binding)) {
                label += ' ';
                label.append(yyjson_get_str(binding), yyjson_get_len(binding));
            }
            stage(label);
        }
    }
    yyjson_doc_free(doc);
}
void Runtime::configure() {
    if (configured_ || api_failed_ || restarting() || !storage_ok_ || !station_.connected()) {
        return;
    }
    if (!api_started_) {
        if (!settings_.valid()) {
            return;
        }
        // Freeze destination identity and the I/O snapshot in one owner turn.
        // A UI draft edit while the transport is full cannot change either.
        std::string destination = std::string(normalized_api_root(settings_.model_url.data())) +
                                  "\n" + settings_.model.data();
        // Preserve existing Chat route identities; Responses is a distinct selection.
        if (std::strcmp(settings_.api_type.data(), "responses") == 0) {
            destination += "\nresponses";
        }
        uint8_t digest[32]{};
        size_t digest_bytes = 0;
        if (psa_crypto_init() != PSA_SUCCESS ||
            psa_hash_compute(PSA_ALG_SHA_256, reinterpret_cast<const uint8_t *>(destination.data()),
                             destination.size(), digest, sizeof(digest),
                             &digest_bytes) != PSA_SUCCESS ||
            digest_bytes != sizeof(digest)) {
            api_failed_ = true;
            session_status(Session::failed, "模型目的地标识计算失败");
            return;
        }
        for (size_t i = 0; i < 8; ++i) {
            std::snprintf(route_.data() + i * 2, 3, "%02x", digest[i]);
        }
        if (!api_.initialize(settings_, transport_)) {
            api_failed_ = true;
            session_status(Session::failed, "API 任务启动失败，请重启");
            return;
        }
        selection_.route = route_;
        selection_.model = settings_.model;
        const auto scope = normalized_api_root(settings_.model_url.data());
        std::snprintf(selection_.scope.data(), selection_.scope.size(), "%.*s", int(scope.size()),
                      scope.data());
        selection_.api = settings_.model_api();
        selection_.chat_token_limit = settings_.model_token_limit();
        api_started_ = true;
    }
    if (transport_.post(Kind::configure, 0, &selection_, sizeof(selection_))) {
        voice_setting_ = VoiceSetting{++next_id_, preferences_.voice_reply(), false};
        configured_ = true;
        api_.submit(Request::health, ++next_id_);
        session_status(Session::opening, "已联网，正在打开 Ayni 会话");
    }
}

void Runtime::clear_preview() {
    active_model_ = 0;
    volatile char *bytes = preview_text_.data();
    for (size_t at = 0; at < preview_text_.size(); ++at) {
        bytes[at] = 0;
    }
    std::string{}.swap(preview_text_);
    std::fill(preview_packet_.text.begin(), preview_packet_.text.end(), 0);
    preview_packet_.size = 0;
    dirty_ = true;
}
void Runtime::cancel() {
    if (motor_.active()) {
        motor_.stop(transport_);
    }
    remote_.cancel_recording();
    api_.cancel_all();
    clear_preview();
    if (capacity_ == Capacity::measuring) {
        message("正在取消容量测试");
        return; // No Core goal or physical operation belongs to this synthetic test.
    }
    if (motor_check_id_) {
        transport_.post(Kind::cancel, *motor_check_id_, nullptr, 0);
        message("正在停止自检，不会自动重试");
        return;
    }
    work_ = busy() ? Work::cancelling : Work::idle;
    if (!transport_.post(Kind::cancel, turn_, nullptr, 0)) {
        message("取消队列已满，请再次点击取消");
    } else {
        message("正在取消，不会自动重试");
    }
}
void Runtime::start_motor_check(uint32_t motor) {
    if (!ready() || busy() || recognizing() || restarting()) {
        message("请先结束当前回合");
        return;
    }
    uint8_t body[sizeof(uint32_t)];
    put32(body, motor);
    const auto id = ++next_id_;
    if (!transport_.post(Kind::motor_check, id, body, sizeof(body))) {
        message("自检队列已满，未执行");
        return;
    }
    motor_check_id_ = id;
    message("已提交本地自检，无需模型");
}
void Runtime::send(std::string_view text, uint64_t id) {
    if (voice_setting_) {
        message("语音开关同步中，尚未发送输入");
        return;
    }
    if (!model_clock_ready()) {
        message("等待网络校时；未创建对话任务");
        return;
    }
    if (!ready() || !station_.connected() || busy() || recognizing() || text.empty() ||
        text.size() > input_bytes || restarting()) {
        message("会话未就绪、忙碌或输入超限，未发送");
        return;
    }
    const bool nonempty = text.find_first_not_of(" \t\r\n") != text.npos;
    if (!nonempty) {
        message("识别为空，未发送");
        return;
    }
    if (!id) {
        id = ++next_id_;
    }
    if (!transport_.post(Kind::user, id, text.data(), text.size())) {
        message("输入队列已满，未发送");
        return;
    }
    turn_ = id;
    speech_error_.clear();
    work_ = Work::running;
    stage_count_ = 0;
    model_round_ = 0;
    for (auto &entry : stages_) {
        entry.clear();
    }
    clear_preview();
    stage("输入已提交给Ayni");
    transcript_.assign(text);
    answer_.clear();
    page_ = Page::history;
    current_page_ = 0;
    message("已发送，Ayni 正在处理");
}
void Runtime::packets() {
    while (transport_.take_output(packet_)) {
        const auto id = packet_.id;
        const std::string_view body(reinterpret_cast<const char *>(packet_.body.data()),
                                    packet_.size);
        switch (packet_.kind) {
        case Kind::voice_reply_state:
            if (voice_setting_ && id == voice_setting_->id && packet_.size == 1 &&
                bool(packet_.body[0]) == voice_setting_->enabled) {
                voice_setting_.reset();
                if (page_ == Page::voice) {
                    message(preferences_.voice_reply() ? "自动语音回复已开启"
                                                       : "自动语音回复已关闭");
                }
            }
            break;
        case Kind::motor_check_state: {
            MotorCheckNotice notice;
            if (!motor_check_id_ || id != *motor_check_id_ ||
                !MotorCheckNotice::decode(packet_.body.data(), packet_.size, notice)) {
                break;
            }
            const bool terminal = notice.phase == MotorCheckNotice::Phase::completed ||
                                  notice.phase == MotorCheckNotice::Phase::failed ||
                                  notice.phase == MotorCheckNotice::Phase::cancelled;
            if (terminal) {
                if (notice.phase != MotorCheckNotice::Phase::completed) {
                    motor_.stop(transport_);
                }
                motor_check_id_.reset();
            }
            motor_.test_notice(notice);
            message(notice.phase == MotorCheckNotice::Phase::failed ? "电机自检失败，详情见电机页"
                    : terminal ? "自检已结束，详情见电机页"
                               : "电机自检中，可随时停止");
            break;
        }
        case Kind::tool_outline:
            if (id == turn_) {
                tool_outline(body);
            }
            break;
        case Kind::motor_state:
            if (motor_.packet(packet_)) {
                dirty_ = true;
            }
            break;
        case Kind::model_request:
        case Kind::template_model_request:
            if (!api_started_ || restarting() || work_ != Work::running ||
                (packet_.kind == Kind::template_model_request &&
                 capacity_ != Capacity::template_turn && capacity_ != Capacity::loop_turn) ||
                !api_.submit(packet_.kind == Kind::template_model_request
                                 ? Request::template_capture
                                 : Request::model,
                             id, body.data(), body.size())) {
                transport_.post(Kind::model_error, id, nullptr, 0);
            } else {
                clear_preview();
                active_model_ = id;
                ++model_round_;
                const bool summary = packet_.size == model_request_bytes &&
                                     get32(packet_.body.data() + body_reference_bytes) == 0;
                stage(std::string(summary ? "整理上下文，模型第" : "模型处理，第") +
                      std::to_string(model_round_) + "轮");
            }
            break;
        case Kind::speech_request:
            if (!api_started_ || restarting() || work_ != Work::running ||
                !api_.submit(Request::speech, id, body.data(), body.size())) {
                transport_.post(Kind::audio_error, id, nullptr, 0);
            } else {
                stage("扬声器调用：正在生成语音");
            }
            break;
        case Kind::audio_cancel:
            api_.cancel_speech(id);
            break;
        case Kind::render_text: {
            stage("屏幕能力：正在显示文档");
            document_turn_ = turn_;
            document_.assign(body);
            current_page_ = 0;
            page_ = Page::conversation;
            canvas_.clear();
            pages_ = canvas_.document(document_, 0, 48, 244);
            draw();
            const uint8_t done = transport_.paint(id, canvas_.pixels(), screen_bytes) ? 1 : 0;
            uint8_t count[4]{};
            put32(count, pages_);
            if (!transport_.post(Kind::render_info, id, count, sizeof(count)) ||
                !transport_.post(Kind::display_ack, id, &done, sizeof(done))) {
                message("显示回执队列已满");
            }
            break;
        }
        case Kind::answer:
            if (id == turn_) {
                answer_.assign(body);
                if (work_ != Work::cancelling) {
                    work_ = Work::running; // A late answer cannot reopen cancelled work.
                }
                if (document_turn_ != id) {
                    page_ = Page::history;
                    current_page_ = 0;
                }
                message("回答已保存");
            }
            break;
        case Kind::status:
            if (body == "answer saving") {
                stage("模型回答完成，保存会话");
            } else if (body == "speaker speech dispatched") {
                stage("扬声器调用已派发");
            } else if (body == "speaker tone dispatched") {
                stage("扬声器提示音已派发");
            } else if (body == "speaker playback queued") {
                stage("收到首段语音，边接收边播放");
            } else if (body.find("task held") == 0) {
                if (capacity_ == Capacity::template_turn || capacity_ == Capacity::loop_turn) {
                    transport_.post(Kind::cancel, turn_, nullptr, 0);
                    work_ = Work::cancelling;
                }
                stage(speech_error_.empty() ? std::string("任务已暂停：") + std::string(body)
                                            : speech_error_ + "；任务已暂停，未重播");
            } else if (body == "motor resources initialized") {
                motor_.initialized(true);
            } else if (body == "motor initialization failed") {
                motor_.initialized(false);
                message("电机资源初始化失败，聊天仍可使用");
            } else if (body == "automatic voice reply draining after error") {
                stage("语音异常，等待停止确认后结束本轮");
            } else if (body == "automatic voice reply started") {
                stage("语音回复：正在朗读最终答复");
            } else if (body.find("motor preparation failed") == 0) {
                motor_.preparation_failed(transport_, body);
                message("电机初始化失败，详情见电机页");
            } else if (body == "ready") {
                session_status(Session::ready, "Ayni 就绪");
                resources(transport_.diagnostics(), "ready");
            } else if (body == "accepted") {
                stage("Ayni已接收，准备对话");
            } else if (body == "preparing input: waiting for body cleanup") {
                stage("正在收尾上一轮资源");
            } else if (body == "preparing input: waiting for input storage") {
                stage("正在保存本次输入");
            } else if (body == "preparing input: creating task") {
                stage("正在创建对话任务");
            } else if (turn_status(body) == TurnStatus::complete) {
                if (id == turn_) {
                    work_ = Work::idle;
                    if (capacity_ == Capacity::template_turn) {
                        if (api_.submit(Request::capacity, ++next_id_)) {
                            capacity_ = Capacity::measuring;
                            message("正在测量合成上下文；不调用模型、不修改会话");
                        } else {
                            capacity_ = Capacity::idle;
                            message("容量测试队列不可用，未开始");
                        }
                    } else if (capacity_ == Capacity::loop_turn) {
                        capacity_ = Capacity::idle;
                        message("完整ToolLoop容量样本已完成；结果见Web日志");
                    } else {
                        message(body.find("reply completed; voice failed") == 0
                                    ? body
                                    : std::string_view("本轮已完成；可继续输入"));
                    }
                }
            } else if (turn_status(body) == TurnStatus::failed) {
                if (id == turn_ &&
                    (capacity_ == Capacity::template_turn || capacity_ == Capacity::loop_turn)) {
                    capacity_ = Capacity::idle;
                }
                if (id == turn_) {
                    work_ = Work::idle;
                }
                message(body);
            } else if (body.find("initialize failed") != body.npos ||
                       body.find("restricted") != body.npos ||
                       body.find("stopped/held") != body.npos) {
                // Preserve the specific initialization/storage cause when the
                // owner subsequently reports its generic stopped/held status.
                // The owner emits a generic NOT_READY after its reliable STOPPED
                // notice. It cannot override the confirmed close needed for rebuild.
                if (session_ != Session::failed && session_ != Session::stopped) {
                    if (motor_.active()) {
                        motor_.stop(transport_);
                    }
                    api_.cancel_all();
                    clear_preview();
                    remote_.cancel_recording();
                    work_ = Work::idle;
                    motor_check_id_.reset();
                    const std::string detail = session_ == Session::opening
                                                   ? session_detail_ + "\n" + std::string(body)
                                                   : std::string(body);
                    session_status(Session::failed, detail);
                }
            } else if (body.find("reset shutdown failed") == 0) {
                message(body);
            } else if (body.find("stopped; reset board") == 0) {
                if (restart_ != Restart::ota) {
                    static_cast<void>(
                        station_.close_radio()); // Failure is logged; no false release.
                }
                if (restart_ == Restart::ota) {
                    session_status(Session::stopped, "会话已关闭，正在接收并校验固件");
                    update_.stopped();
                } else if (restart_ == Restart::rebuild) {
                    session_status(Session::stopped, "Core及存储已关闭，准备重建实验数据");
                } else if (session_ == Session::failed) {
                    session_detail_ += "\n会话已关闭；请复位设备重开，数据已保留";
                    dirty_ = true;
                    message("存储异常已关闭会话，请复位重开；无需擦除数据");
                } else if (restarting()) {
                    esp_restart();
                } else {
                    session_status(Session::closing, "Ayni 已关闭");
                }
            }
            if (session_ == Session::opening &&
                (body.find("stage:") == 0 || body.find("storage open=") == 0)) {
                session_detail_.assign(body);
                dirty_ = true;
            }
            if (body == "context cleared on device") {
                clear_preview();
                transcript_.clear();
                answer_.clear();
                page_ = Page::history;
                current_page_ = 0;
                message("上下文已清空");
            } else if (body.find("context clear") == 0 ||
                       body.find("speaker playback failed") == 0) {
                message(speech_error_.empty() ? body : std::string_view(speech_error_));
            } else if (body == "speaker playback completed") {
                stage("播放已完成，ToolLoop处理工具结果");
            }
            if (body.find("metrics ") == 0 || body.find("motor stage=") == 0) {
                std::printf("%.*s\n", int(body.size()), body.data());
            }
            break;
        default:
            break;
        }
    }
}
void Runtime::results() {
    if (api_.take_preview(preview_packet_) && work_ == Work::running &&
        preview_packet_.id == active_model_) {
        preview_text_.assign(preview_packet_.text.data(), preview_packet_.size);
        dirty_ = true;
    }
    std::fill(preview_packet_.text.begin(), preview_packet_.text.end(), 0);
    preview_packet_.size = 0;
    while (auto *job = api_.result()) {
        if (job->request == Request::capacity) {
            capacity_ = Capacity::idle;
            message(!api_.current(*job) ? "容量测试已取消；不完整结果见Web日志"
                    : job->ok           ? "容量测试完成；结果见Web实时日志"
                                        : job->error.data());
            api_.release(job);
            continue;
        }
        if (!api_.current(*job)) {
            if (job->request == Request::model && active_model_ == job->input.id) {
                clear_preview();
            }
            api_.release(job);
            continue;
        }
        if (job->request == Request::model || job->request == Request::template_capture) {
            const auto ui_dequeued_us = uint64_t(esp_timer_get_time());
            if (active_model_ == job->input.id) {
                clear_preview();
            }
            if (job->ok || (job->output.kind == Kind::model_error &&
                            job->output.size == model_result_header_bytes)) {
                if (!transport_.post(job->output.kind, job->input.id, job->output.body.data(),
                                     job->output.size)) {
                    message("模型结果提交失败，将按超时收尾");
                } else if (job->ok && job->outline.size) {
                    // Best-effort metadata follows the accepted result, so it cannot occupy
                    // the queue slot needed by the authoritative model completion.
                    transport_.post(Kind::tool_outline, job->input.id, job->outline.bytes.data(),
                                    job->outline.size);
                }
            } else {
                transport_.post(Kind::model_error, job->input.id, nullptr, 0);
                message(job->error.data());
            }
            const auto post_ui_us = uint64_t(esp_timer_get_time());
            uint64_t frame_wait_us = 0;
            const auto overlap = last_frame_.classify(job->done_enqueued_us, frame_wait_us);
            const uint64_t pack_us =
                job->http_complete_us && job->done_enqueued_us >= job->http_complete_us
                    ? job->done_enqueued_us - job->http_complete_us
                    : 0;
            const uint64_t ui_wait_us =
                job->done_enqueued_us && ui_dequeued_us >= job->done_enqueued_us
                    ? ui_dequeued_us - job->done_enqueued_us
                    : 0;
            transport_.diagnostics().record(
                "wifi model_handoff turn=%llu call=%llu ok=%u http_us=%llu done_us=%llu ui_us=%llu "
                "post_ui_us=%llu pack_us=%llu ui_wait_us=%llu draw_us=%llu..%llu "
                "paint_us=%llu..%llu ui_frame=%s frame_wait_us=%llu\n",
                static_cast<unsigned long long>(turn_),
                static_cast<unsigned long long>(job->input.id), unsigned(job->ok),
                static_cast<unsigned long long>(job->http_complete_us),
                static_cast<unsigned long long>(job->done_enqueued_us),
                static_cast<unsigned long long>(ui_dequeued_us),
                static_cast<unsigned long long>(post_ui_us),
                static_cast<unsigned long long>(pack_us),
                static_cast<unsigned long long>(ui_wait_us),
                static_cast<unsigned long long>(last_frame_.draw_begin_us),
                static_cast<unsigned long long>(last_frame_.draw_end_us),
                static_cast<unsigned long long>(last_frame_.paint_begin_us),
                static_cast<unsigned long long>(last_frame_.paint_end_us),
                UiFrameWindow::name(overlap), static_cast<unsigned long long>(frame_wait_us));
        } else if (job->request == Request::recognize) {
            work_ = Work::idle;
            if (job->ok) {
                transcript_.assign(reinterpret_cast<const char *>(job->output.body.data()),
                                   job->output.size);
                answer_.clear();
                page_ = Page::history;
                current_page_ = 0;
                send(transcript_, job->input.id);
            } else {
                message(job->error.data());
            }
        } else if (job->request == Request::speech) {
            if (!job->ok) {
                transport_.post(Kind::audio_error, job->input.id, nullptr, 0);
                speech_error_.assign(job->error.data());
                message(speech_error_);
            } else {
                stage("语音流接收完成，等待播放结束");
            }
        } else {
            message(job->ok ? "语音 API 就绪" : job->error.data());
        }
        api_.release(job);
    }
}
void Runtime::save_restart() {
    if (configured_ && (!ready() || busy() || recognizing())) {
        message("请等待会话就绪，或先结束、取消当前回合");
        return;
    }
    if (!storage_ok_ || !settings_.save()) {
        message("配置未保存，请检查 Wi-Fi、HTTP 地址和模型密钥");
        return;
    }
    begin_restart();
}
void Runtime::stop_for_restart() {
    // OTA keeps the RF lease and Wi-Fi alive until the HTTP response has been attempted.
    if (restart_ != Restart::ota && !station_.close_radio()) {
        message("RF唤醒引用释放失败，重启后重新初始化");
    }
    if (motor_.active()) {
        motor_.stop(transport_);
    }
    api_.cancel_all();
    clear_preview();
    remote_.cancel_recording();
    restart_at_ = esp_timer_get_time() + (restart_ == Restart::settings ? 1000000 : 1);
}
void Runtime::update_progress() {
    if (update_.verify_boot(storage_ok_ && touch_ok_ && web_started_ && station_.connected() &&
                            session_ == Session::ready && !restarting())) {
        motor_.stop(transport_);
        api_.cancel_all();
        remote_.cancel_recording();
        const auto result = esp_ota_mark_app_invalid_rollback_and_reboot();
        transport_.diagnostics().record("wifi ota rollback request status=%ld\n", long(result));
        message("新固件启动确认失败，请检查更新日志");
    }
    if (update_.snapshot().phase == UpdatePlan::Phase::requested) {
        if (!ready() || busy() || recognizing() || restarting() || !api_.idle() ||
            remote_snapshot_.state == Remote::State::recording) {
            update_.close_refused(ESP_ERR_INVALID_STATE);
        } else if (update_.begin_close()) {
            if (!transport_.post(Kind::maintenance_stop, 0, nullptr, 0)) {
                update_.close_refused(ESP_ERR_NO_MEM);
            } else {
                restart_ = Restart::ota;
                work_ = Work::cancelling;
                stop_for_restart();
                message("正在关闭会话，随后更新固件");
            }
        }
    }
    if (restart_ == Restart::ota && update_.reboot_due()) {
        static_cast<void>(station_.close_radio());
        esp_restart();
    }
}
void Runtime::begin_restart() {
    restart_ = Restart::settings;
    stop_for_restart();
    message("配置已保存，等待 Ayni 正常关闭后重启");
}
void Runtime::begin_rebuild() {
    if (!ready()) {
        message("请等待会话就绪；未能正常关闭时不会重建");
        return;
    }
    if (!transport_.post(Kind::maintenance_stop, 0, nullptr, 0)) {
        message("停止请求队列已满，未开始重建，请重试");
        return;
    }
    restart_ = Restart::rebuild;
    work_ = Work::cancelling;
    stop_for_restart();
    message("正在停止输出和网络请求，随后关闭会话");
}
void Runtime::reset_progress() {
    if (restart_ != Restart::rebuild || session_ != Session::stopped || !api_.idle()) {
        return;
    }
    if (reset_.state() == ResetWorker::State::idle) {
        const auto status = reset_.start_after_shutdown();
        std::printf("wifi reset start status=%d\n", int(status));
    }
    if (reset_.state() == ResetWorker::State::done) {
        std::printf("wifi reset complete; configuration and pairing partitions preserved\n");
        esp_restart();
    } else if (reset_.state() == ResetWorker::State::failed) {
        char error[96];
        std::snprintf(error, sizeof(error), "重建失败 code=%d，请复位；不会自动重试",
                      int(reset_.error()));
        message(error);
    } else {
        char progress[80];
        std::snprintf(progress, sizeof(progress), "正在重建任务与会话 %lu%%，请勿断电",
                      static_cast<unsigned long>(reset_.percent()));
        message(progress);
    }
}
void Runtime::web_changes() {
    if (!web_started_) {
        return;
    }
    const bool editable = (!configured_ || ready()) && !busy() && !recognizing() && !restarting() &&
                          remote_snapshot_.state != Remote::State::recording;
    web_.editable(editable);
    if (const auto probe = web_.take_probe()) {
        uint8_t size[sizeof(uint32_t)];
        const bool full_loop = probe != UINT32_MAX;
        put32(size, full_loop ? probe : 0);
        if (editable && ready() && api_started_ && api_.idle() &&
            transport_.post(Kind::context_template, ++next_id_, size, sizeof(size))) {
            turn_ = next_id_;
            capacity_ = full_loop ? Capacity::loop_turn : Capacity::template_turn;
            work_ = Work::running;
            web_.editable(false);
            message("正在取得真实工具模板；不调用云端模型、不保存对话");
        } else {
            transport_.diagnostics().record("wifi probe rejected: device not idle/ready\n");
        }
    }
    WebConfig::Patch patch{};
    if (!web_.take(patch)) {
        return;
    }
    auto candidate = settings_;
    for (size_t i = 0; i < Settings::field_count; ++i) {
        if (patch.change.fields & (1U << i)) {
            std::snprintf(candidate.field(i), candidate.capacity(i), "%s",
                          patch.change.values.field(i));
        }
    }
    const bool idle = (!configured_ || ready()) && !busy() && !recognizing() && !restarting() &&
                      remote_snapshot_.state != Remote::State::recording;
    const bool saved =
        idle && esp_timer_get_time() < patch.deadline && candidate.valid() && candidate.save();
    if (saved) {
        settings_ = candidate;
    }
    web_.complete(patch.request_id, saved);
    patch.change.values.password.fill(0);
    patch.change.values.model_key.fill(0);
    candidate.password.fill(0);
    candidate.model_key.fill(0);
    if (saved) {
        begin_restart();
    }
}
void Runtime::tap(int x, int y) {
    if (restarting()) {
        return;
    }
    dirty_ = true;
    if (motor_.active() && y >= 2 && y < 26 && x >= 180 && x < 236) {
        motor_.stop(transport_);
        cancel();
        page_ = Page::motor;
        return;
    }
    if (page_ != Page::launcher && y >= 2 && y < 26 && x >= 4 && x < 52) {
        if (page_ == Page::keyboard) {
            page_ = field_ < Settings::field_count ? Page::settings : Page::history;
        } else if (page_ == Page::networks) {
            page_ = Page::settings;
        } else if (page_ == Page::settings && settings_view_ == SettingsView::options) {
            settings_view_ = SettingsView::model;
        } else {
            page_ = Page::launcher;
        }
        return;
    }
    switch (page_) {
    case Page::launcher:
        if (const auto app = app_at(x, y)) {
            open_app(*app);
        }
        break;
    case Page::voice:
        if (x >= 8 && x < 232 && y >= 84 && y < 160) {
            toggle_voice();
        }
        break;
    case Page::status:
        if (!current_page_ && y >= 66 && y < 260) {
            current_page_ = 1;
        } else if (y >= 270) {
            if (x < 116) {
                page_ = Page::launcher;
                current_page_ = 0;
            } else if (x < 178 && current_page_) {
                --current_page_;
            } else if (x >= 178 && current_page_ + 1 < pages_) {
                ++current_page_;
            }
        }
        break;
    case Page::conversation:
    case Page::history:
        tap_home(x, y);
        break;
    case Page::maintenance:
        if (x >= 8 && x < 232 && y >= 84 && y < 148) {
            page_ = Page::confirm_clear;
        } else if (x >= 8 && x < 232 && y >= 164 && y < 240) {
            begin_rebuild();
        }
        break;
    case Page::settings:
        tap_settings(x, y);
        break;
    case Page::keyboard:
        tap_keyboard(x, y);
        break;
    case Page::networks:
        tap_networks(x, y);
        break;
    case Page::remote:
        tap_remote(x, y);
        break;
    case Page::motor: {
        const auto action = motor_.tap(
            transport_, x, y, ready() && station_.connected() && !busy() && !recognizing());
        if (action == MotorPanel::Tap::test_first || action == MotorPanel::Tap::test_second) {
            start_motor_check(action == MotorPanel::Tap::test_first ? 0 : 1);
        }
    } break;
    default:
        tap_confirmation(x, y);
        break;
    }
}
void Runtime::open_app(App app) {
    current_page_ = 0;
    switch (app) {
    case App::chat:
        page_ = Page::history;
        break;
    case App::voice:
        page_ = Page::voice;
        break;
    case App::wifi:
        settings_view_ = SettingsView::network;
        page_ = Page::settings;
        break;
    case App::model:
        settings_view_ = SettingsView::model;
        page_ = Page::settings;
        break;
    case App::remote:
        page_ = Page::remote;
        break;
    case App::motor:
        page_ = Page::motor;
        break;
    case App::screen:
        page_ = Page::conversation;
        break;
    case App::status:
        page_ = Page::status;
        break;
    case App::maintenance:
        page_ = Page::maintenance;
        break;
    }
}
void Runtime::toggle_voice() {
    if (!preferences_ok_ || !preferences_.set_voice_reply(!preferences_.voice_reply())) {
        voice_hint_ = "保存失败，保持原状态";
        message("语音开关未保存，保持原状态");
        return;
    }
    voice_hint_ = "重启后保留";
    voice_setting_ = VoiceSetting{++next_id_, preferences_.voice_reply(), false};
    message(preferences_.voice_reply() ? "语音回复已开启" : "语音回复已关闭");
}
void Runtime::tap_home(int x, int y) {
    if (y >= 24 && y < 48) {
        page_ = Page::status;
        current_page_ = 0;
    } else if (y >= 252 && y < 282) {
        if (x < 80) {
            page_ = Page::launcher;
        } else if (x < 160) {
            field_ = Settings::field_count;
            editing_.fill(0);
            page_ = Page::keyboard;
        } else {
            cancel();
        }
    } else if (y >= 286) {
        if (x < 80 && current_page_) {
            --current_page_;
        } else if (x >= 80 && x < 160) {
            page_ = page_ == Page::history ? Page::conversation : Page::history;
            current_page_ = 0;
        } else if (x >= 160 && current_page_ + 1 < pages_) {
            ++current_page_;
        }
    }
}
void Runtime::tap_settings(int x, int y) {
    if (settings_view_ == SettingsView::options && y >= preference_top &&
        y < preference_top + 3 * option_pitch) {
        const auto option = (y - preference_top) / option_pitch;
        if ((y - preference_top) % option_pitch >= 52) {
            return;
        }
        if (option == 0) {
            std::strcpy(settings_.api_type.data(),
                        settings_.model_api() == ayni::providers::model_protocol::Api::responses
                            ? "chat_completions"
                            : "responses");
        } else if (option == 1) {
            std::strcpy(settings_.stream.data(), settings_.model_streaming() ? "false" : "true");
        } else {
            std::strcpy(settings_.output_limit.data(),
                        std::strcmp(settings_.output_limit.data(), "max_tokens") == 0
                            ? "max_completion_tokens"
                            : "max_tokens");
        }
        return;
    }
    if (y >= preference_top && y < preference_top + 4 * preference_pitch &&
        settings_view_ != SettingsView::options) {
        const auto index = size_t((y - preference_top) / preference_pitch);
        const auto count = settings_view_ == SettingsView::network ? std::size(network_fields)
                                                                   : std::size(model_fields);
        if (index >= count || (y - preference_top) % preference_pitch >= preference_height) {
            return;
        }
        field_ =
            settings_view_ == SettingsView::network ? network_fields[index] : model_fields[index];
        if (!field_) {
            page_ = Page::networks;
            station_.scan();
        } else {
            std::snprintf(editing_.data(), editing_.size(), "%s", settings_.field(field_));
            page_ = Page::keyboard;
        }
    } else if (y >= 248 && y < 276 && settings_view_ == SettingsView::model) {
        settings_view_ = SettingsView::options;
    } else if (y >= 284) {
        save_restart();
    } else if (y >= 246 && y < 274 && station_.connected() && api_started_) {
        api_.submit(Request::health, ++next_id_);
    }
}
void Runtime::tap_keyboard(int x, int y) {
    if (y >= 122 && y < 266) {
        const auto keys = keyboard_keys(symbols_);
        const size_t index = size_t((y - 122) / 36) * 10 + size_t(x / 24);
        if (index < keys.size()) {
            char key = keys[index];
            if (shifted_ && key >= 'a' && key <= 'z') {
                key = char(key - 'a' + 'A');
            }
            const size_t capacity =
                field_ < Settings::field_count ? settings_.capacity(field_) : editing_.size();
            const auto size = std::strlen(editing_.data());
            if (size + 1 < capacity && size + 1 < editing_.size()) {
                editing_[size] = key;
                editing_[size + 1] = 0;
            }
        }
    } else if (y >= 270) {
        if (x < 48) {
            shifted_ = !shifted_;
        } else if (x < 96) {
            symbols_ = !symbols_;
        } else if (x < 144) {
            const auto size = std::strlen(editing_.data());
            if (size) {
                editing_[size - 1] = 0;
            }
        } else if (x < 192) {
            if (field_ < Settings::field_count) {
                page_ = Page::settings;
            } else {
                page_ = Page::conversation;
            }
        } else {
            if (field_ < Settings::field_count) {
                std::snprintf(settings_.field(field_), settings_.capacity(field_), "%s",
                              editing_.data());
                page_ = Page::settings;
            } else {
                page_ = Page::conversation;
                send(editing_.data());
            }
            editing_.fill(0);
        }
    }
}
void Runtime::tap_networks(int x, int y) {
    if (y >= 62 && y < 270) {
        const auto selected = size_t((y - 62) / 26);
        if (selected < network_count_) {
            std::memcpy(settings_.ssid.data(), networks_[selected].ssid, 32);
            settings_.ssid[32] = 0;
            page_ = Page::settings;
        }
    } else if (y >= 280) {
        if (x < 80) {
            page_ = Page::settings;
        } else if (x < 160) {
            station_.scan();
        } else {
            field_ = 0;
            std::snprintf(editing_.data(), editing_.size(), "%s", settings_.ssid.data());
            page_ = Page::keyboard;
        }
    }
}
void Runtime::tap_remote(int x, int y) {
    if (y >= 24 && y < remote_candidates_top) {
        message(remote_snapshot_.message.data());
        page_ = Page::status;
        return;
    }
    if (y >= remote_candidates_top &&
        y < remote_candidates_top +
                int(remote_snapshot_.candidates.size()) * remote_candidate_pitch) {
        peer_index_ = size_t((y - remote_candidates_top) / remote_candidate_pitch);
        if (peer_index_ < remote_snapshot_.count) {
            page_ = Page::confirm_peer;
        }
    } else if (y >= 282) {
        if (x < 60) {
            page_ = Page::launcher;
        } else if (x < 120) {
            remote_.scan();
        } else if (x < 180) {
            remote_.disconnect();
        } else {
            page_ = Page::confirm_forget;
        }
    }
}
void Runtime::tap_confirmation(int x, int y) {
    if (y < 270) {
        return;
    }
    if (x >= 120) {
        if (page_ == Page::confirm_clear) {
            if (!busy() && !recognizing() && ready()) {
                transport_.post(Kind::forget, ++next_id_, nullptr, 0);
            } else if (!ready()) {
                message("会话未就绪，未清空；点击顶部状态查看原因");
            } else {
                message("正在录音、转写或处理，未清空会话");
            }
        } else if (page_ == Page::confirm_forget) {
            remote_.forget();
        } else if (page_ == Page::confirm_peer && peer_index_ < remote_snapshot_.count) {
            remote_.pair(remote_snapshot_.candidates[peer_index_].address);
        }
    }
    page_ = page_ == Page::confirm_clear ? Page::conversation : Page::remote;
}

void Runtime::draw() {
    canvas_.clear();
    canvas_.label(92, 8, "Ayni", Canvas::muted);
    canvas_.rounded_rectangle(154, 12, 6, 6, 3,
                              station_.connected() ? Canvas::positive : Canvas::muted);
    if (page_ != Page::launcher) {
        canvas_.button(4, 2, 48, 24, "返回");
    }
    switch (page_) {
    case Page::launcher:
        canvas_.label(12, 28, ready() ? "应用" : "应用 · 连接中", Canvas::muted);
        draw_app_menu(canvas_, preferences_.voice_reply());
        break;
    case Page::voice:
        draw_voice_preferences(canvas_, preferences_.voice_reply(),
                               !preferences_ok_ ? "读取失败，原设置保留"
                               : voice_setting_ ? "同步中…"
                                                : voice_hint_);
        break;
    case Page::status:
        draw_status();
        break;
    case Page::conversation:
    case Page::history:
        draw_home();
        break;
    case Page::maintenance:
        draw_maintenance();
        break;
    case Page::settings:
        draw_settings();
        break;
    case Page::keyboard:
        draw_keyboard();
        break;
    case Page::networks:
        draw_networks();
        break;
    case Page::remote:
        draw_remote();
        break;
    case Page::motor:
        motor_.draw(canvas_);
        break;
    default:
        draw_confirmation();
        break;
    }
    if (motor_.active()) {
        canvas_.button(180, 2, 56, 24, "停止", Canvas::danger, Canvas::background);
    }
    dirty_ = false;
}
void Runtime::draw_status() {
    canvas_.heading("设备状态");
    const std::string detail =
        "会话：\n" + session_detail_ + "\n\n遥控器：\n" + remote_snapshot_.message.data() +
        "\n\n模型网络：\n" + (model_clock_ready() ? "校时已就绪" : "等待网络校时，尚不能对话") +
        "\n\n当前提示：\n" + status_ + "\n\n执行过程：\n" + stages(stage_capacity);
    pages_ = 1 + canvas_.document(detail, current_page_ ? current_page_ - 1 : SIZE_MAX, 66, 260);
    if (!current_page_) {
        canvas_.setting_row(66, 44, "会话", ready() ? "就绪" : "未就绪");
        canvas_.setting_row(116, 44, "遥控器",
                            remote_snapshot_.state == Remote::State::ready ? "已连接" : "未就绪");
        canvas_.setting_row(166, 44, "网络校时", model_clock_ready() ? "已同步" : "等待同步");
        canvas_.setting_row(216, 44, "当前状态", status_);
    }
    canvas_.button(8, 278, 104, 36, "返回");
    canvas_.button(118, 278, 54, 36, "上页");
    canvas_.button(178, 278, 54, 36, current_page_ ? "下页" : "详情");
}
void Runtime::draw_home() {
    const std::string_view activity = remote_snapshot_.state == Remote::State::recording
                                          ? "正在录音，松键后转写"
                                      : recognizing()             ? "正在ASR转写"
                                      : work_ == Work::cancelling ? "正在取消"
                                      : !ready()                  ? "会话未就绪，点击查看"
                                      : !model_clock_ready() ? "等待网络校时，暂不能对话"
                                                             : std::string_view(status_);
    canvas_.label(8, 28, activity);
    const auto history =
        busy() && answer_.empty()
            ? "执行过程：\n" + stages(recent_stages) + "\n你：\n" + transcript_ +
                  (preview_text_.empty() ? "" : "\n\n生成中（预览）：\n" + preview_text_)
            : "你：\n" + transcript_ + "\n\nAyni：\n" + answer_;
    const std::string_view content =
        page_ == Page::history ? std::string_view(history) : std::string_view(document_);
    pages_ = canvas_.document(
        content.empty() ? (page_ == Page::history ? "按住语音键，开始对话" : "暂无屏幕内容")
                        : content,
        current_page_, 52, 244);
    const char *navigation[]{"应用", "输入", "取消"};
    const char *pages[]{"上页", page_ == Page::history ? "文档" : "聊天", "下页"};
    for (int at = 0; at < 3; ++at) {
        canvas_.button(at * 80, 252, 76, 30, navigation[at]);
        canvas_.button(at * 80, 286, 76, 30, pages[at]);
    }
}
void Runtime::draw_maintenance() {
    if (restarting()) {
        canvas_.document(status_ + "\n\n重建期间请勿断电。", 0, 40, 270);
        return;
    }
    canvas_.heading("清理");
    canvas_.setting_row(84, 64, "清空对话", "保留任务记录");
    canvas_.setting_row(164, 76, "确认重建任务与会话", "删除任务与对话");
    canvas_.label(18, 168, "确认重建任务与会话", Canvas::danger);
    canvas_.label(16, 256, "保留网络、密钥及配对", Canvas::muted);
}
void Runtime::draw_settings() {
    std::array<PreferenceRow, 4> rows{};
    size_t count = 0;
    if (settings_view_ == SettingsView::options) {
        const char *titles[]{"接口", "流式预览", "Chat 输出字段"};
        const char *values[]{
            settings_.model_api() == ayni::providers::model_protocol::Api::responses
                ? "Responses"
                : "Chat Completions",
            settings_.model_streaming() ? "开启" : "关闭", settings_.output_limit.data()};
        count = std::size(titles);
        for (size_t at = 0; at < count; ++at) {
            rows[at] = {titles[at], values[at]};
        }
    } else {
        count = settings_view_ == SettingsView::network ? std::size(network_fields)
                                                        : std::size(model_fields);
        for (size_t at = 0; at < count; ++at) {
            const auto field =
                settings_view_ == SettingsView::network ? network_fields[at] : model_fields[at];
            rows[at] = {labels[field], (field == 1 || field == 4)
                                           ? (*settings_.field(field) ? "已设置" : "未设置")
                                           : settings_.field(field)};
        }
    }
    const auto url = WebConfig::address();
    draw_endpoint_preferences(canvas_,
                              settings_view_ == SettingsView::network ? "网络"
                              : settings_view_ == SettingsView::model ? "模型与语音"
                                                                      : "模型选项",
                              rows.data(), count, settings_view_ == SettingsView::options,
                              settings_view_ == SettingsView::model, url.data());
}
void Runtime::draw_keyboard() {
    canvas_.heading(field_ < Settings::field_count ? labels[field_] : "输入消息");
    std::string shown = field_ == 1 || field_ == 4 ? std::string(std::strlen(editing_.data()), '*')
                                                   : editing_.data();
    if (shown.size() > 28) {
        shown = shown.substr(shown.size() - 28);
    }
    canvas_.rounded_rectangle(8, 64, 224, 42, 8, Canvas::surface);
    canvas_.label(16, 78, shown, Canvas::accent, 208);
    const auto keys = keyboard_keys(symbols_);
    for (int i = 0; i < 40; ++i) {
        char key[2]{size_t(i) < keys.size() ? keys[i] : ' ', 0};
        if (shifted_ && key[0] >= 'a' && key[0] <= 'z') {
            key[0] = char(key[0] - 'a' + 'A');
        }
        canvas_.button((i % 10) * 24, 122 + (i / 10) * 36, 23, 34, key);
    }
    const char *buttons[] = {"Aa", "符号", "退格", "返回", "完成"};
    for (int i = 0; i < 5; ++i) {
        canvas_.button(i * 48, 270, 46, 44, buttons[i]);
    }
}
void Runtime::draw_networks() {
    canvas_.heading("Wi-Fi 网络");
    for (size_t i = 0; i < network_count_; ++i) {
        canvas_.button(8, 62 + int(i) * 26, 224, 24, {});
        canvas_.label(16, 66 + int(i) * 26, reinterpret_cast<const char *>(networks_[i].ssid),
                      Canvas::foreground, 208);
    }
    if (!network_count_) {
        canvas_.label(16, 100, "暂无网络，点击扫描", Canvas::muted);
    }
    canvas_.button(0, 280, 78, 34, "返回");
    canvas_.button(81, 280, 78, 34, "扫描");
    canvas_.button(162, 280, 78, 34, "手输");
}
void Runtime::draw_remote() {
    canvas_.heading("遥控器");
    const char *state = remote_snapshot_.state == Remote::State::ready       ? "已连接"
                        : remote_snapshot_.state == Remote::State::recording ? "录音中"
                        : remote_snapshot_.state == Remote::State::scanning  ? "扫描中…"
                        : remote_snapshot_.state == Remote::State::failed ? "连接失败 · 查看状态"
                        : remote_snapshot_.state == Remote::State::disconnected ? "未连接"
                                                                                : "连接中…";
    canvas_.label(12, 66, state,
                  remote_snapshot_.state == Remote::State::ready ? Canvas::positive
                                                                 : Canvas::muted);
    for (size_t i = 0; i < remote_snapshot_.count; ++i) {
        const auto &candidate = remote_snapshot_.candidates[i];
        const auto *a = candidate.address.val;
        char address[32]{};
        std::snprintf(address, sizeof(address), "%02X:%02X:%02X:%02X:%02X:%02X", a[5], a[4], a[3],
                      a[2], a[1], a[0]);
        const int top = remote_candidates_top + int(i) * remote_candidate_pitch;
        canvas_.setting_row(top, 42, candidate.name.data(), address);
    }
    const char *buttons[] = {"返回", "扫描", "断开", "忘记"};
    for (int i = 0; i < 4; ++i) {
        canvas_.button(i * 60, 282, 58, 34, buttons[i]);
    }
}
void Runtime::draw_confirmation() {
    const char *prompt =
        page_ == Page::confirm_clear ? "清空已完成会话的上下文？\n不会删除 Core 日志。"
        : page_ == Page::confirm_forget ? "忘记当前遥控器的蓝牙配对？\n之后需要重新配对。"
                                        : "连接所选遥控器？\n请确认名称和地址对应手边设备。";
    canvas_.document(prompt, 0, 60, 244);
    canvas_.button(0, 270, 116, 44, "返回");
    canvas_.button(122, 270, 116, 44, "确认");
}

void Runtime::run() {
    update_.initialize(transport_.diagnostics());
    storage_ok_ = settings_.load();
    preferences_ok_ = storage_ok_ && preferences_.load();
    if (!preferences_ok_) {
        message("语音偏好读取失败；原数据保留");
    }
    touch_ok_ = touch_.initialize();
    if (!touch_ok_) {
        message("触摸初始化失败，请检查连线");
    }
    if (!storage_ok_) {
        message("配置分区不可用；未擦除任何数据");
    }
    if (!station_.initialize(transport_.diagnostics())) {
        message("Wi-Fi 初始化失败");
    }
    if (storage_ok_) {
        web_started_ = web_.initialize(settings_, transport_.diagnostics(), update_);
        if (!web_started_) {
            message("Web 配置服务启动失败");
        }
    }
    if (storage_ok_ && !remote_.initialize()) {
        message("蓝牙初始化失败");
    }
    if (storage_ok_ && settings_.network_valid()) {
        if (station_.connect(settings_)) {
            message("正在连接 Wi-Fi，稍后自动打开会话");
        } else {
            message("Wi-Fi 连接未启动，请检查设置");
        }
    }
    resources(transport_.diagnostics(), "startup");
    int64_t drawn = 0;
    for (;;) {
        const auto now = esp_timer_get_time();
        if (!station_.poll()) {
            message("Wi-Fi RF策略未就绪，连接已取消");
        }
        const bool connected = station_.connected();
        const bool clock_ready = model_clock_ready();
        if (clock_ready != was_clock_ready_) {
            was_clock_ready_ = clock_ready;
            dirty_ = true;
            if (clock_ready && ready() && !busy() && !recognizing() && !restarting()) {
                message("网络校时完成，Ayni 就绪");
            }
        }
        if (connected != was_connected_) {
            if (!connected) {
                cancel();
                message("Wi-Fi 已断开，未自动重试请求；保存重启可重连");
            }
            was_connected_ = connected;
            dirty_ = true;
            if (connected && !settings_.valid()) {
                message("已联网，请在设置页查看 Web 地址");
            }
        }
        configure();
        packets();
        if (configured_ && voice_setting_ && !voice_setting_->queued) {
            const uint8_t enabled = voice_setting_->enabled ? 1 : 0;
            if (transport_.post(Kind::voice_reply_setting, voice_setting_->id, &enabled,
                                sizeof(enabled))) {
                voice_setting_->queued = true;
            }
        }
        if (api_started_) {
            results();
        }
        if (station_.networks(networks_, network_count_)) {
            dirty_ = true;
        }
        const auto remote = remote_.snapshot();
        if (remote_snapshot_.revision != remote.revision) {
            remote_snapshot_ = remote;
            dirty_ = true;
            if (remote.state == Remote::State::recording) {
                page_ = Page::history;
                current_page_ = 0;
                message(remote.message.data());
            }
        }
        if (motor_.tick(transport_, ready() && connected && !restarting(), now)) {
            dirty_ = true;
        }
        update_progress();
        web_changes();
        if (restart_at_ && now >= restart_at_ && api_.idle()) {
            if (!configured_) {
                if (restart_ == Restart::settings) {
                    esp_restart();
                }
            } else if (transport_.post(restart_ == Restart::rebuild || restart_ == Restart::ota
                                           ? Kind::maintenance_shutdown
                                           : Kind::shutdown,
                                       0, nullptr, 0)) {
                if (restart_ == Restart::rebuild) {
                    std::printf("wifi reset api=idle; shutdown requested\n");
                }
                restart_at_ = 0;
            }
        }
        reset_progress();
        remote_.allow_recording(ready() && clock_ready && connected && !busy() && !recognizing() &&
                                !restarting() && remote_snapshot_.state == Remote::State::ready &&
                                page_ != Page::keyboard);
        uint8_t *recording = nullptr;
        size_t bytes = 0;
        if (remote_.take_audio(recording, bytes)) {
            if (ready() && clock_ready && connected && !busy() && !recognizing() && !restarting() &&
                api_started_ &&
                api_.submit(Request::recognize, ++next_id_, nullptr, 0, recording, bytes)) {
                work_ = Work::recognizing;
                message("松键录音完成，正在 ASR 转写");
            } else {
                std::memset(recording, 0, bytes);
                heap_caps_free(recording);
                message("当前忙碌，本次录音未发送");
            }
        }
        int x = 0, y = 0;
        if (touch_ok_ && touch_.released(x, y)) {
            tap(x, y);
        }
        if (dirty_ && now - drawn >= redraw_us) {
            const bool first_frame = drawn == 0;
            const auto draw_begin_us = uint64_t(esp_timer_get_time());
            draw();
            const auto draw_end_us = uint64_t(esp_timer_get_time());
            const bool painted = transport_.local_display().paint(canvas_.pixels(), screen_bytes);
            const auto paint_end_us = uint64_t(esp_timer_get_time());
            last_frame_ = {draw_begin_us, draw_end_us, draw_end_us, paint_end_us};
            if (!painted) {
                dirty_ = true;
            }
            if (first_frame) {
                resources(transport_.diagnostics(), "first-frame");
            }
            drawn = now;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
} // namespace
void run(Transport &transport) {
    auto *memory = heap_caps_malloc(sizeof(Runtime), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!memory) {
        vTaskSuspend(nullptr);
        return;
    }
    auto *runtime = new (memory) Runtime(transport);
    runtime->run();
}
} // namespace wifichat::wifi
