#include "configuration.h"
#include <ayni/ayni.hpp>
#include <ayni/context.h>
#include <ayni/host_authority.h>
#include <ayni/hosts/sim.h>
#include <gtest/gtest.h>

// ADR-0100: the actual ESP deployment must durably open the shared Session's Core anchor.
TEST(WifiConfiguration, DeploymentOpensUsableConversationContextThroughPublicApi) {
    using ayni::initialized;
    constexpr uint64_t owner = 1, agent_id = 1, tick_us = 10000;
    constexpr uint32_t max_steps = 200, step_items = 8;
    auto config = wifichat::chat_config(AYNI_CALL_INPUT_BYTES_DEFAULT, wifichat::summary_bytes);
    ayni::hosts::sim::Host host;
    ayni::Kernel kernel;
    const ayni::Quiesce quiesce{
        &host, [](void *context) -> ayni_status_t {
            return static_cast<ayni::hosts::sim::Host *>(context)->pending_storage() == 0
                       ? AYNI_OK
                       : AYNI_BUSY;
        }};
    ASSERT_EQ(kernel.create(config, host.table(), quiesce), AYNI_OK);
    const ayni_principal_desc_v1 principal{sizeof(principal), AYNI_PRINCIPAL_OWNER, owner, 1, 0};
    const ayni_agent_desc_v1 agent{sizeof(agent), 0, agent_id};
    ASSERT_EQ(ayni_register_principal(kernel.get(), &principal), AYNI_OK);
    ASSERT_EQ(ayni_register_agent(kernel.get(), &agent), AYNI_OK);
    const ayni::Progress progress{&host, [](void *context) -> ayni_status_t {
                                      auto &sim = *static_cast<ayni::hosts::sim::Host *>(context);
                                      sim.advance_us(tick_us);
                                      sim.complete_storage();
                                      return AYNI_OK;
                                  }};
    auto until = [&](auto done) {
        for (uint32_t step = 0; step < max_steps; ++step) {
            if (done()) {
                return true;
            }
            progress.advance(progress.context);
            auto report = initialized<ayni_step_report_v1>();
            if (kernel.step(step_items, tick_us, report) != AYNI_OK) {
                return false;
            }
        }
        return done();
    };
    ASSERT_EQ(kernel.start(), AYNI_OK);
    uint64_t session = 0;
    const ayni_session_desc_v1 session_desc{sizeof(session_desc), AYNI_CHANNEL_MANAGEMENT, owner, 1,
                                            0};
    ASSERT_TRUE(
        until([&] { return ayni_open_session(kernel.get(), &session_desc, &session) == AYNI_OK; }));
    auto info = initialized<ayni_agent_info_v1>();
    ASSERT_EQ(ayni_get_agent(kernel.get(), agent_id, &info), AYNI_OK);
    auto command = initialized<ayni_context_command_v1>();
    command.owner_instance_id = info.instance_id;
    ASSERT_EQ(kernel.submit_raw(AYNI_INTENT_CONTEXT_OPEN, 1, session, 0, command).status, AYNI_OK);
    auto context = initialized<ayni_context_info_v1>();
    ASSERT_TRUE(until([&] {
        return ayni_get_context(kernel.get(), session, info.instance_id, &context) == AYNI_OK &&
               context.usable == 1;
    }));
    EXPECT_EQ(context.owner_instance_id, info.instance_id);
    EXPECT_EQ(context.pending_action, AYNI_CONTEXT_IDLE);
    ASSERT_EQ(kernel.drain(max_steps, progress), AYNI_OK);
    EXPECT_EQ(kernel.close(), AYNI_OK);
}
