#include "work/authority/authority_facts.h"

#include "foundation/kernel_id.h"
namespace ayni::work {

bool valid_authority_reference(const AuthorityReference &reference) {
    return reference.grant_id == 0U
               ? reference.revision == 0U
               : foundation::is_kernel_id(reference.grant_id) && reference.revision != 0U;
}

bool valid_selected_authority_reference(const AuthorityReference &reference) {
    return reference.grant_id != 0U && valid_authority_reference(reference);
}

bool valid_binding_reference(const BindingReference &reference) {
    return reference.goal_id == 0U
               ? reference.commit_seq == 0U
               : foundation::is_kernel_id(reference.goal_id) && reference.commit_seq != 0U;
}

bool valid_input_resume_boundary(const InputResumeBoundary &boundary) {
    return boundary.incarnation == 0U ? boundary.through == 0U
                                      : boundary.incarnation <= foundation::max_boot_epoch;
}

bool input_precedes_resume(const InputResumeBoundary &input, const InputResumeBoundary &resume) {
    return resume.incarnation != 0U && input.incarnation == resume.incarnation &&
           input.through <= resume.through;
}

bool valid_goal_binding(const GoalBinding &binding, uint64_t goal_id, uint64_t parent_goal_id,
                        foundation::TransitionStamp stamp) {
    return foundation::is_kernel_id(goal_id) && stamp.transition_id != 0U &&
           binding.established_seq != 0U && binding.established_seq <= stamp.commit_seq &&
           valid_binding_reference(binding.parent) && binding.parent.goal_id == parent_goal_id &&
           parent_goal_id != goal_id && binding.parent.commit_seq < binding.established_seq &&
           valid_input_resume_boundary(binding.input);
}

} // namespace ayni::work
