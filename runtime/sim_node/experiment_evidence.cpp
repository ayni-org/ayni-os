// Records diagnostic copies and the first causal failure; owns no domain state.
#include "experiment_support.h"
#include "fixture_json.h"
namespace ayni::runtime::experiments {
namespace {
constexpr uint32_t evidence_limit = 20000;
}
void Evidence::failure(const std::string &message, ayni_status_t status) {
    if (failure_.empty()) {
        failure_ = message;
        status_ = status;
    }
}
void Evidence::fact(const char *category, const char *name, uint64_t value) {
    record(category, name, std::to_string(value));
}
void Evidence::record(const char *category, const char *name, const std::string &value) {
    if (lines_.size() == evidence_limit) {
        failure("trace capacity");
        return;
    }
    lines_.push_back("{\"category\":" + fixtures::quoted(category) + ",\"name\":" +
                     fixtures::quoted(name) + ",\"value\":" + fixtures::quoted(value) +
                     ",\"sequence\":" + fixtures::quoted(std::to_string(lines_.size() + 1)) +
                     ",\"step\":" + fixtures::quoted(std::to_string(step_)) +
                     ",\"clock_us\":" + fixtures::quoted(std::to_string(clock_us_)) + "}");
}
bool Evidence::status(const char *name, ayni_status_t actual, ayni_status_t expected) {
    record("status", name, std::to_string(actual));
    if (actual != expected) {
        failure(std::string(name) + " status=" + std::to_string(actual) +
                    " expected=" + std::to_string(expected),
                actual == AYNI_OK ? AYNI_INVALID_STATE : actual);
        return false;
    }
    return true;
}
void Evidence::changed(const std::string &name, uint64_t value) {
    changed_value(name, std::to_string(value));
}
void Evidence::changed_status(const std::string &name, ayni_status_t value) {
    changed_value(name, std::to_string(value));
}
void Evidence::changed_value(const std::string &name, const std::string &value) {
    const auto found = snapshots_.find(name);
    if (found == snapshots_.end() || found->second != value) {
        snapshots_[name] = value;
        record("state", name.c_str(), value);
    }
}
bool Evidence::equal(const char *name, uint64_t actual, uint64_t expected) {
    fact("assertion", name, actual);
    fact("assertion.expected", name, expected);
    fact("assertion.passed", name, actual == expected ? 1 : 0);
    if (actual != expected) {
        failure(std::string(name) + " actual=" + std::to_string(actual) +
                " expected=" + std::to_string(expected));
        return false;
    }
    return true;
}
} // namespace ayni::runtime::experiments
