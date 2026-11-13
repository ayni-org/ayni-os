#pragma once
#include <cstdint>
namespace wifichat {
// The Gate owns nonce validity. This extra board interlock rejects challenges issued before
// ordinary disarm, even if UI re-enables before Core finishes dispatching an earlier action.
class MotorAdmission {
  public:
    void issued(uint64_t nonce) { issued_ = nonce; }
    void disable() { refused_ = issued_; }
    bool permits(uint64_t nonce) const { return nonce > refused_; }

  private:
    uint64_t issued_ = 0;  // Last Gate-issued nonce; 0 before any challenge.
    uint64_t refused_ = 0; // Inclusive disable barrier, never greater than issued_.
};
} // namespace wifichat
