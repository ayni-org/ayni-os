#include "runtime.h"
#include "../main/speaker.h"
#include "../main/turn_status.h"
#include "../main/wake.h"
#include "api.h"
#include "canvas.h"
#include "motor_panel.h"
#include "remote.h"
#include "reset_worker.h"
#include "station.h"
#include "touch.h"
#include "web_config.h"
#include <algorithm>
#include <ayni/model_messages.hpp>
#include <cstdio>
#include <cstring>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <new>
#include <psa/crypto.h>
#include <string>
#include <yyjson.h>
namespace wifichat::wifi {
namespace {
constexpr int64_t redraw_us = 300000;
constexpr size_t recent_stages = 3;
constexpr int remote_candidates_top = 90, remote_candidate_pitch = 46;
constexpr const char *labels[] = {"Wi-Fi 名称",      "Wi-Fi 密码",   "语音 API 地址",
                                  "模型 API 根地址", "模型 API Key", "模型名称"};
std::string_view keyboard_keys(bool symbols) {
    return symbols ? "!@#$%^&*()_+-=[]{}:<>?~`|\\\"' ABCDEFGHIJK"
                   : "qwertyuiopasdfghjkl;zxcvbnm,./0123456789";
}
void resources(const char *stage) {
    std::printf(
        "wifi resources %s internal_free=%zu psram_free=%zu psram_largest=%zu io_stack_free=%lu\n",
        stage, heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
        static_cast<unsigned long>(uxTaskGetStackHighWaterMark(nullptr)));
}
enum class Page {
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
    enum class Restart { none, settings, rebuild };
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
    void configure();
    void cancel();
    void send(std::string_view text, uint64_t id = 0);
    void save_restart();
    void web_changes();
    void begin_restart();
    void begin_rebuild();
    void stop_for_restart();
    void reset_progress();
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
    Canvas canvas_{};
    Touch touch_{};
    Station station_{};
    Remote remote_{};
    MotorPanel motor_{};
    Api api_{};
    WebConfig web_{};
    bool web_started_ = false;
    Packet packet_{};
    bool storage_ok_ = false, touch_ok_ = false, configured_ = false;
    enum class Session { unconfigured, opening, ready, failed, closing, stopped };
    Session session_ = Session::unconfigured;
    bool ready() const { return session_ == Session::ready; }
    void session_status(Session state, std::string_view detail) {
        session_ = state;
        session_detail_.assign(detail);
        message(detail);
        // This is boot/session metadata, never model output or a transcript.
        std::printf("wifi session=%u %.*s\n", unsigned(state), int(detail.size()), detail.data());
    }
    enum class Work { idle, recognizing, running, cancelling };
    Work work_ = Work::idle; // A new user turn cannot overlap recognition or cancellation.
    bool busy() const { return work_ == Work::running || work_ == Work::cancelling; }
    bool recognizing() const { return work_ == Work::recognizing; }
    bool api_started_ = false, api_failed_ = false, dirty_ = true;
    Restart restart_ = Restart::none;
    ResetWorker reset_;
    std::array<char, 17> route_{}; // Frozen with Api's active settings, separate from the UI draft.
    bool shifted_ = false, symbols_ = false, was_connected_ = false;
    Page page_ = Page::conversation;
    size_t field_ = 0, current_page_ = 0, pages_ = 1, peer_index_ = 0;
    uint64_t document_turn_ = 0; // Last turn that explicitly used the screen capability.
    uint64_t turn_ = 0, next_id_ = uint64_t(esp_random()) << 32U;
    static constexpr size_t stage_capacity = 8;
    std::array<std::string, stage_capacity> stages_{};
    size_t stage_count_ = 0;
    uint32_t model_round_ = 0;
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
        const std::string destination =
            std::string(normalized_api_root(settings_.model_url.data())) + "\n" +
            settings_.model.data();
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
        api_started_ = true;
    }
    if (transport_.post(Kind::configure, 0, route_.data(), route_.size() - 1)) {
        configured_ = true;
        api_.submit(Request::health, ++next_id_);
        session_status(Session::opening, "已联网，正在打开 Ayni 会话");
    }
}

void Runtime::cancel() {
    if (motor_.active()) {
        motor_.stop(transport_);
    }
    remote_.cancel_recording();
    api_.cancel_all();
    work_ = busy() ? Work::cancelling : Work::idle;
    if (!transport_.post(Kind::cancel, turn_, nullptr, 0)) {
        message("取消队列已满，请再次点击取消");
    } else {
        message("正在取消，不会自动重试");
    }
}
void Runtime::send(std::string_view text, uint64_t id) {
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
            if (!api_started_ || restarting() || work_ != Work::running ||
                !api_.submit(Request::model, id, body.data(), body.size())) {
                transport_.post(Kind::model_error, id, nullptr, 0);
            } else {
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
                stage(speech_error_.empty() ? std::string("任务已暂停：") + std::string(body)
                                            : speech_error_ + "；任务已暂停，未重播");
            } else if (body.find("motor preparation failed") == 0) {
                motor_.preparation_failed(transport_);
                message("电机准备失败，请重启；未执行动作");
            } else if (body == "ready") {
                session_status(Session::ready, "Ayni 就绪");
                resources("ready");
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
                    message("本轮已完成；可继续输入");
                }
            } else if (turn_status(body) == TurnStatus::failed) {
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
                    remote_.cancel_recording();
                    work_ = Work::idle;
                    const std::string detail = session_ == Session::opening
                                                   ? session_detail_ + "\n" + std::string(body)
                                                   : std::string(body);
                    session_status(Session::failed, detail);
                }
            } else if (body.find("reset shutdown failed") == 0) {
                message(body);
            } else if (body.find("stopped; reset board") == 0) {
                static_cast<void>(station_.close_radio()); // Failure is logged; no false release.
                if (restart_ == Restart::rebuild) {
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
            if (body.find("metrics ") == 0) {
                std::printf("%.*s\n", int(body.size()), body.data());
            }
            break;
        default:
            break;
        }
    }
}
void Runtime::results() {
    while (auto *job = api_.result()) {
        if (!api_.current(*job)) {
            api_.release(job);
            continue;
        }
        if (job->request == Request::model) {
            if (job->ok) {
                if (!transport_.post(job->output.kind, job->input.id, job->output.body.data(),
                                     job->output.size)) {
                    message("模型结果提交失败，将按超时收尾");
                }
            } else {
                transport_.post(Kind::model_error, job->input.id, nullptr, 0);
                message(job->error.data());
            }
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
    if (!station_.close_radio()) {
        message("RF唤醒引用释放失败，重启后重新初始化");
    }
    if (motor_.active()) {
        motor_.stop(transport_);
    }
    api_.cancel_all();
    remote_.cancel_recording();
    restart_at_ = esp_timer_get_time() + (restart_ == Restart::settings ? 1000000 : 1);
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
    web_.editable((!configured_ || ready()) && !busy() && !recognizing() && !restarting() &&
                  remote_snapshot_.state != Remote::State::recording);
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
    if (motor_.active() && y < 24 && x >= 178) {
        motor_.stop(transport_);
        cancel();
        page_ = Page::motor;
        return;
    }
    switch (page_) {
    case Page::status:
        if (y >= 270) {
            if (x < 116) {
                page_ = Page::conversation;
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
        if (y >= 278) {
            page_ = Page::history;
        } else if (y >= 186 && y < 222) {
            page_ = Page::confirm_clear;
        } else if (y >= 228 && y < 268) {
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
    case Page::motor:
        if (motor_.tap(transport_, x, y, ready() && station_.connected()) ==
            MotorPanel::Tap::back) {
            page_ = Page::history;
        }
        break;
    default:
        tap_confirmation(x, y);
        break;
    }
}
void Runtime::tap_home(int x, int y) {
    if (y < 24) {
        page_ = page_ == Page::history ? Page::conversation : Page::history;
        current_page_ = 0;
    }
    if (y >= 24 && y < 48) {
        page_ = Page::status;
        current_page_ = 0;
    }
    if (y >= 252 && y < 282) {
        if (x < 60) {
            page_ = Page::settings;
        } else if (x < 120) {
            page_ = Page::remote;
        } else if (x < 180) {
            page_ = Page::motor;
        } else {
            field_ = Settings::field_count;
            editing_.fill(0);
            page_ = Page::keyboard;
        }
    } else if (y >= 286) {
        if (x < 60) {
            cancel();
        } else if (x < 120) {
            page_ = Page::maintenance;
        } else if (x < 180 && current_page_) {
            --current_page_;
        } else if (x >= 180 && current_page_ + 1 < pages_) {
            ++current_page_;
        }
    }
}
void Runtime::tap_settings(int x, int y) {
    if (y >= 30 && y < 246) {
        field_ = size_t((y - 30) / 36);
        if (!field_) {
            page_ = Page::networks;
            station_.scan();
        } else {
            std::snprintf(editing_.data(), editing_.size(), "%s", settings_.field(field_));
            page_ = Page::keyboard;
        }
    } else if (y >= 276) {
        if (x < 100) {
            page_ = Page::conversation;
        } else {
            save_restart();
        }
    } else if (y >= 246) {
        if (station_.connected() && api_started_) {
            api_.submit(Request::health, ++next_id_);
        } else {
            message("Wi-Fi 未连接，请保存配置后重启");
        }
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
    if (y >= 40 && y < 264) {
        const auto selected = size_t((y - 40) / 28);
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
            page_ = Page::conversation;
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
    canvas_.label(8, 6, station_.connected() ? "Ayni · Wi-Fi 已连接" : "Ayni · Wi-Fi 未连接");
    switch (page_) {
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
        canvas_.button(178, 0, 60, 24, "停止");
    }
    dirty_ = false;
}
void Runtime::draw_status() {
    const std::string detail = "会话：\n" + session_detail_ + "\n\n遥控器：\n" +
                               remote_snapshot_.message.data() + "\n\n当前提示：\n" + status_ +
                               "\n\n执行过程：\n" + stages(stage_capacity);
    pages_ = canvas_.document(detail, current_page_, 32, 250);
    canvas_.button(0, 270, 114, 44, "返回");
    canvas_.button(118, 270, 58, 44, "上页");
    canvas_.button(180, 270, 58, 44, "下页");
}
void Runtime::draw_home() {
    const std::string_view activity = remote_snapshot_.state == Remote::State::recording
                                          ? "正在录音，松键后转写"
                                      : recognizing()             ? "正在ASR转写"
                                      : work_ == Work::cancelling ? "正在取消"
                                      : !ready()                  ? "会话未就绪，点击查看"
                                                                  : std::string_view(status_);
    canvas_.label(8, 28, activity);
    const auto history = busy() && answer_.empty()
                             ? "执行过程：\n" + stages(recent_stages) + "\n你：\n" + transcript_
                             : "你：\n" + transcript_ + "\n\nAyni：\n" + answer_;
    const std::string_view content =
        page_ == Page::history ? std::string_view(history) : std::string_view(document_);
    pages_ = canvas_.document(
        content.empty()
            ? "按住遥控器语音键说话。\n松开后自动转写并提交。\n\n首次使用：设置 Wi-Fi 和 "
              "API，再扫描配对遥控器。\n点击顶栏切换会话和屏幕文档。\n此配置未启用电机。"
            : content,
        current_page_, 52, 244);
    const char *navigation[] = {"设置", "遥控器", "电机", "输入"};
    for (int i = 0; i < 4; ++i) {
        canvas_.button(i * 60, 252, 58, 30, navigation[i]);
    }
    const char *buttons[] = {"取消", "清理", "上页", "下页"};
    for (int i = 0; i < 4; ++i) {
        canvas_.button(i * 60, 286, 58, 30, buttons[i]);
    }
}
void Runtime::draw_maintenance() {
    if (restarting()) {
        canvas_.document(status_ + "\n\n重建期间请勿断电。", 0, 40, 270);
        return;
    }
    canvas_.label(8, 30, "清理任务与会话");
    canvas_.document("仅清对话：保留任务记录。\n重建：删除任务、会话和实验历史。\n保留Wi-"
                     "Fi、模型Key和遥控器配对。\n确认后先停止输出，再重建。",
                     0, 54, 180);
    canvas_.button(8, 186, 224, 34, "仅清空对话");
    canvas_.button(8, 228, 224, 40, "确认重建任务与会话");
    canvas_.button(8, 278, 224, 36, "返回");
}
void Runtime::draw_settings() {
    for (size_t i = 0; i < Settings::field_count; ++i) {
        const int y = 30 + int(i) * 36;
        canvas_.label(8, y, labels[i]);
        canvas_.label(8, y + 16,
                      (i == 1 || i == 4) ? (*settings_.field(i) ? "********" : "未设置")
                                         : settings_.field(i),
                      Canvas::accent);
    }
    const auto url = WebConfig::address();
    canvas_.button(0, 246, 240, 26, *url.data() ? url.data() : "联网后显示 Web 配置地址");
    canvas_.button(0, 278, 96, 36, "返回");
    canvas_.button(100, 278, 140, 36, "保存并重启");
}
void Runtime::draw_keyboard() {
    canvas_.label(8, 30,
                  field_ < Settings::field_count ? labels[field_] : "输入消息（中文请用遥控器）");
    std::string shown = field_ == 1 || field_ == 4 ? std::string(std::strlen(editing_.data()), '*')
                                                   : editing_.data();
    if (shown.size() > 28) {
        shown = shown.substr(shown.size() - 28);
    }
    canvas_.label(8, 58, shown);
    canvas_.label(8, 84, "完成只更新本页；配置需保存重启");
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
    canvas_.label(8, 26, "选择 2.4 GHz Wi-Fi");
    for (size_t i = 0; i < network_count_; ++i) {
        canvas_.button(4, 40 + int(i) * 28, 232, 26,
                       reinterpret_cast<const char *>(networks_[i].ssid));
    }
    canvas_.button(0, 280, 78, 34, "返回");
    canvas_.button(81, 280, 78, 34, "扫描");
    canvas_.button(162, 280, 78, 34, "手输");
}
void Runtime::draw_remote() {
    canvas_.document(remote_snapshot_.message.data(), 0, 28, remote_candidates_top);
    for (size_t i = 0; i < remote_snapshot_.count; ++i) {
        const auto &candidate = remote_snapshot_.candidates[i];
        const auto *a = candidate.address.val;
        char address[32]{};
        std::snprintf(address, sizeof(address), "%02X:%02X:%02X:%02X:%02X:%02X", a[5], a[4], a[3],
                      a[2], a[1], a[0]);
        const int top = remote_candidates_top + int(i) * remote_candidate_pitch;
        canvas_.button(4, top, 232, 42, {});
        canvas_.label(12, top + 3, candidate.name.data());
        canvas_.label(12, top + 23, address);
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
    storage_ok_ = settings_.load();
    touch_ok_ = touch_.initialize();
    if (!touch_ok_) {
        message("触摸初始化失败，请检查连线");
    }
    if (!storage_ok_) {
        message("配置分区不可用；未擦除任何数据");
    }
    if (!station_.initialize()) {
        message("Wi-Fi 初始化失败");
    }
    if (storage_ok_) {
        web_started_ = web_.initialize(settings_);
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
    resources("startup");
    int64_t drawn = 0;
    for (;;) {
        const auto now = esp_timer_get_time();
        if (!station_.poll()) {
            message("Wi-Fi RF策略未就绪，连接已取消");
        }
        const bool connected = station_.connected();
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
        web_changes();
        if (restart_at_ && now >= restart_at_ && api_.idle()) {
            if (!configured_) {
                if (restart_ == Restart::settings) {
                    esp_restart();
                }
            } else if (transport_.post(restart_ == Restart::rebuild ? Kind::maintenance_shutdown
                                                                    : Kind::shutdown,
                                       0, nullptr, 0)) {
                if (restart_ == Restart::rebuild) {
                    std::printf("wifi reset api=idle; shutdown requested\n");
                }
                restart_at_ = 0;
            }
        }
        reset_progress();
        remote_.allow_recording(ready() && connected && !busy() && !recognizing() &&
                                !restarting() && remote_snapshot_.state == Remote::State::ready &&
                                page_ != Page::keyboard);
        uint8_t *recording = nullptr;
        size_t bytes = 0;
        if (remote_.take_audio(recording, bytes)) {
            if (ready() && connected && !busy() && !recognizing() && !restarting() &&
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
            draw();
            if (!transport_.local_display().paint(canvas_.pixels(), screen_bytes)) {
                dirty_ = true;
            }
            if (first_frame) {
                resources("first-frame");
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
