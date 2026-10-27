// Who may submit each intent kind and how a retry of its command is
// recognised (C-WIRE-003, ADR-0026, ADR-0051). These two properties decide
// authorization and idempotency, so every rule that depends on them reads
// this table instead of keeping its own list of kinds, and a new kind is
// reviewed as one row. How a kind is decoded, encoded and applied stays with
// that kind's own code.

#ifndef AYNI_CORE_KERNEL_COMMAND_INTENT_TRAITS_H
#define AYNI_CORE_KERNEL_COMMAND_INTENT_TRAITS_H

#include <ayni/ayni.h>

#include <array>
#include <cstddef>
#include <cstdint>

namespace ayni::kernel {

// The sessions that may submit a kind.
enum class SessionRule : uint8_t {
    any, // any open session of a registered principal (ADR-0017)
    // The durable owner's MANAGEMENT session, checked first among the
    // operation's own rules: a retry of an accepted command returns its
    // first result before that check, as for any command.
    management,
    // The same session, checked before the command is looked up, so a
    // retry needs it too (ayni.h, step 4).
    management_on_retry,
};

// How a retry of a kind's command is recognised.
enum class CommandIdentity : uint8_t {
    ordinary,      // a digest of the decoded intent; command_id is optional
    exact,         // the canonical request bytes; command_id is required
    authorization, // exact, and the mapping also keeps the work authorization result
    revoke,        // exact, held in a pending control until it is durable (ADR-0031)
};

struct IntentTraits {
    ayni_intent_kind_t kind;
    SessionRule session;
    CommandIdentity identity;
    ayni_object_kind_t object; // what an accepted command's result names
};

// Kinds run from GOAL_CREATE to this one without gaps.
constexpr ayni_intent_kind_t last_intent_kind = AYNI_INTENT_PAUSE_GROUP_SET;
constexpr std::size_t intent_kind_count = last_intent_kind - AYNI_INTENT_GOAL_CREATE + 1U;

constexpr std::array<IntentTraits, intent_kind_count> intent_traits_table{{
    {AYNI_INTENT_GOAL_CREATE, SessionRule::any, CommandIdentity::ordinary, AYNI_OBJECT_GOAL},
    {AYNI_INTENT_GOAL_CANCEL, SessionRule::any, CommandIdentity::ordinary, AYNI_OBJECT_GOAL},
    {AYNI_INTENT_GOAL_PAUSE, SessionRule::any, CommandIdentity::ordinary, AYNI_OBJECT_GOAL},
    {AYNI_INTENT_GOAL_RESUME, SessionRule::any, CommandIdentity::ordinary, AYNI_OBJECT_GOAL},
    {AYNI_INTENT_TASK_CANCEL, SessionRule::any, CommandIdentity::ordinary, AYNI_OBJECT_TASK},
    {AYNI_INTENT_INSTANCE_PAUSE, SessionRule::any, CommandIdentity::ordinary,
     AYNI_OBJECT_AGENT_INSTANCE},
    {AYNI_INTENT_INSTANCE_RESUME, SessionRule::any, CommandIdentity::ordinary,
     AYNI_OBJECT_AGENT_INSTANCE},
    {AYNI_INTENT_INSTANCE_STOP, SessionRule::any, CommandIdentity::ordinary,
     AYNI_OBJECT_AGENT_INSTANCE},
    {AYNI_INTENT_GRANT_CREATE, SessionRule::management, CommandIdentity::exact, AYNI_OBJECT_GRANT},
    {AYNI_INTENT_GRANT_REVOKE, SessionRule::management, CommandIdentity::revoke, AYNI_OBJECT_GRANT},
    {AYNI_INTENT_GOAL_APPROVE, SessionRule::management, CommandIdentity::authorization,
     AYNI_OBJECT_GOAL},
    {AYNI_INTENT_GOAL_REAUTHORIZE, SessionRule::management_on_retry, CommandIdentity::authorization,
     AYNI_OBJECT_GOAL},
    {AYNI_INTENT_TASK_RAISE_MODEL_LIMIT, SessionRule::management_on_retry, CommandIdentity::exact,
     AYNI_OBJECT_TASK},
    {AYNI_INTENT_CALL_QUERY_RESULT, SessionRule::management_on_retry, CommandIdentity::exact,
     AYNI_OBJECT_CALL},
    {AYNI_INTENT_TASK_RESUME_WAIT, SessionRule::management_on_retry, CommandIdentity::exact,
     AYNI_OBJECT_TASK},
    {AYNI_INTENT_SCENE_SET_ENABLED, SessionRule::management_on_retry, CommandIdentity::exact,
     AYNI_OBJECT_SCENE},
    {AYNI_INTENT_MEMORY_SAVE, SessionRule::management_on_retry, CommandIdentity::exact,
     AYNI_OBJECT_MEMORY},
    {AYNI_INTENT_MEMORY_CONFIRM, SessionRule::management_on_retry, CommandIdentity::exact,
     AYNI_OBJECT_MEMORY},
    {AYNI_INTENT_MEMORY_DELETE, SessionRule::management_on_retry, CommandIdentity::exact,
     AYNI_OBJECT_MEMORY},
    {AYNI_INTENT_CONTEXT_OPEN, SessionRule::management_on_retry, CommandIdentity::exact,
     AYNI_OBJECT_CONTEXT},
    {AYNI_INTENT_CONTEXT_PUBLISH, SessionRule::management_on_retry, CommandIdentity::exact,
     AYNI_OBJECT_CONTEXT},
    {AYNI_INTENT_CONTEXT_FORGET, SessionRule::management_on_retry, CommandIdentity::exact,
     AYNI_OBJECT_CONTEXT},
    {AYNI_INTENT_CONTEXT_ACK, SessionRule::management_on_retry, CommandIdentity::exact,
     AYNI_OBJECT_CONTEXT},
    {AYNI_INTENT_PAUSE_GROUP_SET, SessionRule::management_on_retry, CommandIdentity::exact,
     AYNI_OBJECT_PAUSE_GROUP},
}};

// Each kind's row sits at its own position, so a lookup is an index.
constexpr bool rows_follow_kinds() {
    for (std::size_t index = 0; index < intent_traits_table.size(); ++index) {
        if (intent_traits_table[index].kind != index + AYNI_INTENT_GOAL_CREATE) {
            return false;
        }
    }
    return true;
}
static_assert(rows_follow_kinds(), "one row per intent kind, in kind order");

// Whether this library implements the kind.
constexpr bool implements_intent(ayni_intent_kind_t kind) {
    return kind >= AYNI_INTENT_GOAL_CREATE && kind <= last_intent_kind;
}

// kind is implemented.
constexpr const IntentTraits &intent_traits(ayni_intent_kind_t kind) {
    return intent_traits_table[kind - AYNI_INTENT_GOAL_CREATE];
}

// These predicates take any kind, including one read from storage.
constexpr bool is_ordinary_command(ayni_intent_kind_t kind) {
    return implements_intent(kind) && intent_traits(kind).identity == CommandIdentity::ordinary;
}

constexpr bool is_exact_command(ayni_intent_kind_t kind) {
    return implements_intent(kind) && intent_traits(kind).identity != CommandIdentity::ordinary;
}

// These exact commands create a new binding and keep its result.
constexpr bool is_work_authority_command(ayni_intent_kind_t kind) {
    return implements_intent(kind) &&
           intent_traits(kind).identity == CommandIdentity::authorization;
}

} // namespace ayni::kernel

#endif // AYNI_CORE_KERNEL_COMMAND_INTENT_TRAITS_H
