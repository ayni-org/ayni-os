// Deterministic model-answer planning, separate from Provider borrow/publication lifecycle.
#ifndef AYNI_PROVIDER_MOCK_TOOL_PLAN_H
#define AYNI_PROVIDER_MOCK_TOOL_PLAN_H
#include <ayni/calls.h>
#include <memory>
namespace ayni::providers::mock {
struct ToolAnswer {
    ayni_status_t status = AYNI_OK;
    uint32_t count = 0;
    const ayni_tool_call_v1 *calls = nullptr; // Borrowed until next answer/clear.
};
class ToolPlanner {
  public:
    ToolPlanner();
    ~ToolPlanner();
    // Startup-only bounded storage. No allocation in prepare/answer/clear.
    ayni_status_t configure(uint32_t capacity, uint32_t text_limit, uint32_t result_limit);
    // Copies offered identities and source arguments during submit. body borrows the
    // Provider slot until clear; the tool callback is never retained or called later.
    void prepare(uint32_t slot, const ayni_provider_request_v1 &, ayni_string_v1 body);
    ToolAnswer answer(uint32_t slot);
    void clear(uint32_t slot); // Clears source content and ends the slot borrow.
  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace ayni::providers::mock
#endif
