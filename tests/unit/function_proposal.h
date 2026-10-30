// Test-only generic function envelope; owns nodes and borrows immutable text.
#ifndef AYNI_TEST_FUNCTION_PROPOSAL_H
#define AYNI_TEST_FUNCTION_PROPOSAL_H
#include <array>
#include <ayni/model_messages.hpp>
#include <gtest/gtest.h>
namespace ayni::test {
class FunctionProposal {
  public:
    FunctionProposal(ayni_string_v1 binding, ayni_arguments_v1 arguments,
                     ayni_string_v1 id = {"test_id", 7}) {
        constexpr uint32_t envelope_nodes = 3;
        EXPECT_LE(arguments.node_count, AYNI_VALUE_NODE_COUNT_MAX - envelope_nodes);
        if (arguments.node_count > AYNI_VALUE_NODE_COUNT_MAX - envelope_nodes) {
            return;
        }
        nodes[0].kind = AYNI_ARGUMENT_OBJECT;
        nodes[0].subtree_end = arguments.node_count + envelope_nodes;
        nodes[1].kind = AYNI_ARGUMENT_STRING;
        nodes[1].subtree_end = 2;
        nodes[1].name = {"binding_id", sizeof("binding_id") - 1U};
        nodes[1].value.text = binding;
        nodes[2].kind = AYNI_ARGUMENT_NUMBER;
        nodes[2].subtree_end = envelope_nodes;
        nodes[2].name = {"binding_version", sizeof("binding_version") - 1U};
        nodes[2].value.text = {"1", 1};
        for (uint32_t at = 0; at < arguments.node_count; ++at) {
            nodes[envelope_nodes + at] = arguments.nodes[at];
            nodes[envelope_nodes + at].subtree_end += envelope_nodes;
        }
        nodes[envelope_nodes].name = {"arguments", sizeof("arguments") - 1U};
        proposal.struct_size = sizeof(proposal);
        proposal.input_kind = AYNI_TOOL_INPUT_ARGUMENTS;
        proposal.function_name = {model::call_function.data(),
                                  static_cast<uint32_t>(model::call_function.size())};
        proposal.protocol_id = id;
        proposal.input.arguments = {nodes.data(), nodes[0].subtree_end};
    }
    ayni_tool_call_v1 &call() { return proposal; }

  private:
    std::array<ayni_argument_node_v1, AYNI_VALUE_NODE_COUNT_MAX> nodes{};
    ayni_tool_call_v1 proposal{};
};
} // namespace ayni::test
#endif
