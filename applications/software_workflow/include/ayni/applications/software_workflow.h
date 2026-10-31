// Public, platform-independent software application registration. No Host or Provider selection.
#ifndef AYNI_APPLICATIONS_SOFTWARE_WORKFLOW_H
#define AYNI_APPLICATIONS_SOFTWARE_WORKFLOW_H
#include <ayni/ayni.h>
namespace ayni::applications::software_workflow {
// CONFIGURING, owner thread. Registers example.managed / example.operations.
// Requires software.sum/events bindings and explicit Grants. Context borrows an ayni_workspace_v1
// value-workspace descriptor through Kernel destroy. Each Task starts a sum and stream, checks the
// exact result and a fresh sample, closes the stream and completes; its one-byte checkpoint owns
// observation evidence.
ayni_status_t configure(void *, ayni_kernel_t *kernel);
} // namespace ayni::applications::software_workflow
#endif
