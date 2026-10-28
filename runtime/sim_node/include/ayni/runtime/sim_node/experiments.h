// Compiled deterministic fault experiments. No scenario contacts a real device
// or model. Data and callbacks have process lifetime; the runner owns each World.
#ifndef AYNI_RUNTIME_SIM_NODE_EXPERIMENTS_H
#define AYNI_RUNTIME_SIM_NODE_EXPERIMENTS_H
#include <ayni/ayni.h>
#include <ayni/calls.h>
#include <cstddef>
#include <string>
namespace ayni::runtime::experiments {
class World;
enum class Deployment { counter, physical, shared, shared_physical, replacement };
struct Case {
    const char *id;
    const char *experiment_step;
    const char *source;
    const char *clauses;
    ayni_call_query_authority_t query_authority;
    Deployment deployment; // Finite simulated profile; no hardware access.
    void (*execute)(World &);
};
struct Cases {
    const Case *data;
    std::size_t size;
};
Cases cases();
const char *source_revision();
struct Result {
    ayni_status_t status = AYNI_OK;
    ayni_status_t close_status = AYNI_OK;
    ayni_status_t artifact_status = AYNI_OK;
    std::string detail; // First causal failure, never a business body.
};
// Requires an entry from cases() and a new output directory. Existing directories
// are never overwritten. Cleanup and artifact failures independently fail the run.
Result run(const Case &, const char *output_directory);
} // namespace ayni::runtime::experiments
#endif
