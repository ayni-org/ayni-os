#include "response_limit.h"
#include <ayni/providers/model_protocol.h>
#include <gtest/gtest.h>
#include <memory>
#include <string>
using namespace wifichat::wifi;
using namespace ayni::providers::model_protocol;
namespace {
constexpr size_t old_wire_limit = 128 * 1024, buffer_limit = 4096;
const std::string terminal =
    "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n"
    "data: [DONE]\n\n";
std::string delta(std::string content) {
    return "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"" + content +
           "\"},\"finish_reason\":null}],\"padding\":\"" + std::string(192, 'p') + "\"}\n\n";
}
} // namespace
// ADR-0097/0098: complete cumulative SSE can exceed the old wire cap with bounded resident output.
TEST(WifiResponse, ModelStreamAcceptsLargeWireBodyWithSmallCompletedAnswer) {
    RequestOptions options;
    options.model = "fixture";
    options.stream = true;
    Stream parser(options, buffer_limit);
    const ResponseLimit wire{};
    const auto chunk = delta("a");
    constexpr size_t events = 1024;
    ASSERT_TRUE(wire.accepts(chunk.size() * events + terminal.size()));
    size_t total = 0;
    for (size_t at = 0; at < events; ++at) {
        ASSERT_TRUE(wire.received(total, chunk.size()));
        ASSERT_EQ(parser.feed(chunk), AYNI_OK);
    }
    ASSERT_GT(total, old_wire_limit);
    ASSERT_TRUE(wire.received(total, terminal.size()));
    ASSERT_EQ(parser.feed(terminal), AYNI_OK);
    auto answer = std::make_unique<Answer>();
    parser.finish(*answer);
    ASSERT_EQ(answer->result.status, AYNI_OK);
    EXPECT_EQ(answer->result.outcome, AYNI_CALL_COMPLETED);
    EXPECT_EQ(std::string_view(answer->result.result.data, answer->result.result.size),
              std::string(events, 'a'));
}
// Same HTTP policy: removing model SSE's wire cap cannot relax buffered JSON or audio bounds.
TEST(WifiResponse, BufferedAndAudioWireBoundsAndCounterOverflowRemainExplicit) {
    const ResponseLimit bounded{buffer_limit};
    EXPECT_TRUE(bounded.accepts(buffer_limit));
    EXPECT_FALSE(bounded.accepts(buffer_limit + 1));
    size_t total = 0;
    ASSERT_TRUE(bounded.received(total, buffer_limit));
    EXPECT_FALSE(bounded.received(total, 1));
    const ResponseLimit model{};
    total = std::numeric_limits<size_t>::max();
    EXPECT_FALSE(model.received(total, 1));
    EXPECT_EQ(total, std::numeric_limits<size_t>::max());
}
// C-MODEL/ADR-0098: a line or assembled output still cannot grow beyond the shared parser bound.
TEST(WifiResponse, ModelParserRetainsSingleEventAndAssembledOutputMemoryProtection) {
    RequestOptions options;
    options.model = "fixture";
    options.stream = true;
    Stream oversized_event(options, buffer_limit);
    EXPECT_EQ(oversized_event.feed(delta(std::string(buffer_limit, 'a'))), AYNI_CAPACITY_EXCEEDED);
    Stream assembled(options, buffer_limit);
    const auto piece = delta(std::string(buffer_limit / 2, 'b'));
    ASSERT_EQ(assembled.feed(piece), AYNI_OK);
    ASSERT_EQ(assembled.feed(piece), AYNI_OK);
    EXPECT_EQ(assembled.feed(delta("b")), AYNI_CAPACITY_EXCEEDED);
}
