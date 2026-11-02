// Explicit manual invocation only. Two small calls; no private inputs or vendor
// response are printed. Credentials are read only by the Provider via its env name.
#include "../unit/openai_fixture.h"
#include <iostream>

namespace {
using namespace ayni::test;
class LiveProvider : public OpenaiFixture {
  protected:
    void SetUp() override {
        const char *base = std::getenv("AYNI_LLM_BASE_URL");
        const char *model = std::getenv("AYNI_LLM_MODEL");
        ASSERT_NE(base, nullptr);
        ASSERT_NE(model, nullptr);
        Config config;
        config.base_url = base;
        config.model = model;
        config.key_environment = "AYNI_LLM_API_KEY";
        config.request_capacity = 1;
        config.max_output_tokens = 128;
        initialize(config);
    }
};
TEST_F(LiveProvider, text_then_function) {
    prompt = "Reply with exactly AYNI_OK. Do not call any tool.";
    create();
    wait_done(40);
    ASSERT_EQ(seen_result.outcome, AYNI_CALL_COMPLETED)
        << "status=" << seen_result.status << " reason=" << seen_result.reason;
    ASSERT_EQ(seen_result.tool_call_count, 0U);
    ASSERT_TRUE(reply == "AYNI_OK");
    seen_result = {};
    reply.clear();
    prompt = "Call tool_0 exactly once with field_1=7, field_2=calm, text=hello. No text reply.";
    create();
    wait_done(40);
    ASSERT_EQ(seen_result.outcome, AYNI_CALL_COMPLETED)
        << "status=" << seen_result.status << " reason=" << seen_result.reason;
    ASSERT_EQ(seen_result.tool_call_count, 1U);
    ASSERT_TRUE(proposal == "show|hello|7second|calm");
}
} // namespace

int main(int argc, char **argv) {
    if (argc != 2 || std::string(argv[1]) != "--live") {
        std::cerr << "Explicit invocation required: ayni_openai_smoke --live\n";
        return 2;
    }
    ::testing::InitGoogleTest();
    return RUN_ALL_TESTS();
}
