#pragma once
namespace wifichat::wifi {
// UI-owned live preferences, stored separately from the frozen endpoint/credential snapshot.
// Old firmware can ignore this namespace without losing its saved network configuration.
class Preferences {
  public:
    bool load();
    bool voice_reply() const { return voice_reply_; }
    bool set_voice_reply(bool enabled);

  private:
    bool voice_reply_ = false; // Missing preference selects opt-in automatic reply speech.
};
} // namespace wifichat::wifi
