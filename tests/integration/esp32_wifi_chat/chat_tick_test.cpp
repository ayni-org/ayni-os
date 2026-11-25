// ADR-0114: actual Chat::tick and reliable terminal drain with a permanently full UI queue.
#include "chat.h"
#include "work_node.h"
#include <ayni/workspace.hpp>
#include <future>
#include <gtest/gtest.h>
namespace wifichat {
struct ChatTickTestPeer {
    static void configure(Chat &chat, ayni_kernel_t *kernel, const ayni_workspace_v1 *workspace) {
        ayni::runtime::OperationSession::Commands commands;
        commands.submit = [](void *, ayni_intent_kind_t, const void *, uint64_t *, uint32_t) {
            return ayni::runtime::OperationSession::SubmitResult{AYNI_DENIED, AYNI_REASON_NONE};
        };
        ASSERT_EQ(chat.motor_.configure_session(kernel, workspace, commands), AYNI_OK);
    }
    static void attach(Chat &chat, ayni_kernel_t *kernel) {
        chat.kernel_ = kernel;
        chat.closing_ = Chat::Closing::quiescing;
        chat.turn_ = 42;
        chat.turn_state_ = Chat::Turn::reply_done;
        chat.reply_end_ = "reply completed";
    }
    static uint64_t steps(const Chat &chat) { return chat.timing_.step_calls; }
    static bool waiting(const Chat &chat) { return chat.turn_state_ == Chat::Turn::reply_done; }
    static bool drain(Chat &chat) { return chat.drain_terminal(); }
    static void expire_drain(Chat &chat) { chat.terminal_drain_deadline_ = 1; }
    static void detach(Chat &chat) { chat.kernel_ = nullptr; }
};
// Hardware/ESP platform endpoints are unavailable in this host fixture. The production owner,
// conversation lifecycle, Core, operation orchestration, and Transport are compiled unchanged.
bool Speaker::initialize() {
    return false;
}
ayni_status_t Speaker::configure(ayni_kernel_t *) {
    return AYNI_NOT_READY;
}
ayni_status_t Speaker::advance() {
    return AYNI_OK;
}
bool Speaker::close() {
    return true;
}
void Speaker::request_stop() {}
uint32_t Speaker::stack_free_bytes() const {
    return 0;
}
uint32_t Speaker::input_bytes() {
    return 1024;
}
} // namespace wifichat
namespace ayni::hosts::esp_idf {
Host::Host() = default;
Host::~Host() = default;
ayni_host_v1 Host::table() {
    return {};
}
StorageOpen Host::open_storage(const char *, uint32_t, bool, StorageWorkHistory *) {
    return {StorageStatus::task_error, 0};
}
void Host::close_storage() {}
storage::FileValuePort Host::value_store() {
    return {};
}
uint64_t Host::append_durable_us(uint64_t) const {
    return 0;
}
bool Host::read_storage_work(uint64_t, StorageWorkSample &) const {
    return false;
}
uint32_t Host::worker_stack_free_bytes() const {
    return 0;
}
StorageMetrics Host::storage_metrics() const {
    return {};
}
StorageMetrics Host::storage_metrics(uint64_t &at) const {
    at = 0;
    return {};
}
} // namespace ayni::hosts::esp_idf
TEST(ChatTick, FullOutputFor200TicksStillStepsCoreAndRetainsTerminalUntilUiDrains) {
    using namespace wifichat;
    ayni::test::WorkNode work;
    ControlMailbox mailbox;
    Transport transport(mailbox);
    ASSERT_TRUE(transport.initialize());
    Chat chat(transport);
    ayni::Workspace workspace;
    ASSERT_EQ(workspace.create(ayni_value_workspace_requirements), AYNI_OK);
    work.node().config().goal_parameter_max_bytes = 1024;
    work.node().config().strategy_capacity = 4;
    work.node().config().goal_type_capacity = 6;
    work.additional_registration = [&](ayni_kernel_t *kernel) {
        ChatTickTestPeer::configure(chat, kernel, workspace.get());
    };
    work.run();
    ChatTickTestPeer::attach(chat, work.node().kernel());
    for (size_t at = 0; at < output_packet_slots; ++at) {
        ASSERT_TRUE(transport.send(Kind::status, at, nullptr, 0));
    }
    transport.diagnostics().record("bounded owner metadata\n");
    std::promise<void> serial_started, release_serial;
    auto release = release_serial.get_future();
    auto logger = std::async(std::launch::async, [&] {
        transport.diagnostics().drain_serial([&](const char *) {
            serial_started.set_value();
            release.wait(); // A stalled UART must not retain any owner/control lock.
        });
    });
    serial_started.get_future().wait();
    for (unsigned at = 0; at < 200; ++at) {
        ayni_step_report_v1 report{};
        bool again = false;
        EXPECT_EQ(chat.tick(report, again), AYNI_OK);
        // Counter is incremented only around the real ayni_step call.
        EXPECT_EQ(ChatTickTestPeer::steps(chat), at + 1U);
        EXPECT_TRUE(ChatTickTestPeer::waiting(chat));
    }
    release_serial.set_value();
    logger.get();
    EXPECT_FALSE(ChatTickTestPeer::drain(chat));
    Packet packet;
    while (transport.take_output(packet)) {
    }
    ayni_step_report_v1 report{};
    bool again = false;
    ASSERT_EQ(chat.tick(report, again), AYNI_OK);
    EXPECT_FALSE(ChatTickTestPeer::waiting(chat));
    ASSERT_TRUE(transport.take_output(packet));
    EXPECT_EQ(packet.kind, Kind::status);
    EXPECT_EQ(packet.id, 42U);
    ChatTickTestPeer::detach(chat);
}
TEST(ChatTick, CloseDrainRetainsTerminalThenExpiresExplicitlyWithoutUiCapacity) {
    using namespace wifichat;
    ControlMailbox mailbox;
    Transport transport(mailbox);
    ASSERT_TRUE(transport.initialize());
    Chat chat(transport);
    ChatTickTestPeer::attach(chat, nullptr);
    EXPECT_FALSE(ChatTickTestPeer::drain(chat));
    EXPECT_TRUE(ChatTickTestPeer::waiting(chat));
    ChatTickTestPeer::expire_drain(chat);
    EXPECT_TRUE(ChatTickTestPeer::drain(chat));
    EXPECT_FALSE(ChatTickTestPeer::waiting(chat));
}
