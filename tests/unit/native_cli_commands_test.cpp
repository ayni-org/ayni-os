// ADR-0115: Native management grammar and bodyless output use the public operation view.
#include "commands.h"
#include "format.h"
#include <gtest/gtest.h>
namespace {
TEST(NativeCliCommands, ParsesIndependentLedgerCommandsAndRejectsAmbiguousArguments) {
    ayni::cli::Request request;
    ASSERT_TRUE(ayni::cli::parse("/unresolved", request));
    EXPECT_EQ(request.command.operation, ayni::runtime::Operation::unresolved_ops);
    ASSERT_TRUE(ayni::cli::parse("/ack 4294967301", request));
    EXPECT_EQ(request.command.operation, ayni::runtime::Operation::acknowledge_op);
    EXPECT_EQ(request.command.id, 4294967301ULL);
    ASSERT_TRUE(ayni::cli::parse("/query 4294967301", request));
    EXPECT_EQ(request.command.operation, ayni::runtime::Operation::query_result);
    EXPECT_FALSE(ayni::cli::parse("/ack", request));
    EXPECT_FALSE(ayni::cli::parse("/ack 1 extra", request));
    EXPECT_FALSE(ayni::cli::parse("/unresolved extra", request));
}
TEST(NativeCliCommands, FormatsRetiredOperationWithoutLookingUpItsTaskOrPrivateBodies) {
    ayni::runtime::Reply reply;
    ayni_unresolved_op_v1 op{};
    op.logical_call_id = 123;
    op.operation_id = 456;
    op.task_id = 789;
    op.revision = 2;
    op.binding_size = 3;
    op.binding_id[0] = 'o';
    op.binding_id[1] = 'l';
    op.binding_id[2] = 'd';
    op.outcome = AYNI_CALL_ABANDONED;
    op.status = AYNI_RESULT_UNKNOWN;
    op.query_authority = AYNI_CALL_QUERY_NONE;
    reply.unresolved_ops.push_back(op);
    const auto output = ayni::cli::format(reply, ayni::cli::Action::execute,
                                          ayni::runtime::Operation::unresolved_ops);
    EXPECT_NE(output.find("Unresolved 123"), std::string::npos);
    EXPECT_NE(output.find("operation=456"), std::string::npos);
    EXPECT_NE(output.find("task=789"), std::string::npos);
    EXPECT_NE(output.find("binding=old"), std::string::npos);
    EXPECT_NE(output.find("revision=2"), std::string::npos);
    EXPECT_NE(output.find("query_authority=none (ack only)"), std::string::npos);
}
} // namespace
