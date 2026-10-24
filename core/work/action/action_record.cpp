#include "work/action/action_record.h"

#include "foundation/kernel_id.h"

namespace ayni::work {
namespace {
bool valid_stamp(ActionStamp stamp) {
    return stamp.boot_epoch != 0U && stamp.boot_epoch <= foundation::max_boot_epoch &&
           stamp.transition.transition_id != 0U && stamp.transition.commit_seq != 0U;
}

bool ordered(ActionStamp before, ActionStamp after) {
    return before.boot_epoch <= after.boot_epoch &&
           before.transition.transition_id < after.transition.transition_id &&
           before.transition.commit_seq < after.transition.commit_seq;
}

bool same_stamp(ActionStamp left, ActionStamp right) {
    return left.boot_epoch == right.boot_epoch &&
           left.transition.transition_id == right.transition.transition_id &&
           left.transition.commit_seq == right.transition.commit_seq;
}

bool known_effect(ActionEffect effect) {
    return effect == ActionEffect::none || effect == ActionEffect::partial ||
           effect == ActionEffect::complete || effect == ActionEffect::unknown;
}

bool final_result(ActionEvidence evidence) {
    return evidence == ActionEvidence::provider_result ||
           evidence == ActionEvidence::authoritative_result;
}
} // namespace

// A shape validator, not proof of historical durability or cross-table binding.
// The replay coordinator must validate those associations before publication.
bool ActionRecord::valid(const ActionFact &fact) {
    const auto &id = fact.identity;
    if (!foundation::is_kernel_id(id.operation_id) ||
        !foundation::is_kernel_id(id.logical_call_id) || id.operation_id == id.logical_call_id ||
        !catalog::CatalogId::is_valid({id.binding_id.data(), id.binding_id.size()}) ||
        id.binding_version == 0U || !valid_stamp(fact.prepared) || !valid_stamp(fact.latest) ||
        foundation::id_boot_epoch(id.operation_id) != fact.prepared.boot_epoch ||
        foundation::id_boot_epoch(id.logical_call_id) > fact.prepared.boot_epoch ||
        !known_effect(fact.effect)) {
        return false;
    }
    if (fact.phase == ActionPhase::prepared) {
        return fact.effect == ActionEffect::none && fact.evidence == ActionEvidence::none &&
               same_stamp(fact.prepared, fact.latest);
    }
    if (!ordered(fact.prepared, fact.latest)) {
        return false;
    }
    switch (fact.phase) {
    case ActionPhase::dispatching:
    case ActionPhase::in_progress:
    case ActionPhase::result_unknown:
        return fact.effect == ActionEffect::unknown && fact.evidence == ActionEvidence::none;
    case ActionPhase::completed:
        return fact.effect == ActionEffect::complete && final_result(fact.evidence);
    case ActionPhase::cancelled:
        return fact.effect == ActionEffect::none &&
               fact.evidence == ActionEvidence::local_non_delivery;
    case ActionPhase::failed:
        // A known failure may still have effects; only authoritative
        // nonacceptance distinguishes a sealed, never-accepted operation.
        return final_result(fact.evidence) ||
               (fact.evidence == ActionEvidence::authoritative_not_accepted &&
                fact.effect == ActionEffect::none);
    default:
        return false;
    }
}

ayni_status_t ActionRecord::prepare(const ActionIdentity &identity, ActionStamp stamp,
                                    std::optional<ActionRecord> &out) {
    const ActionFact fact{
        identity, ActionPhase::prepared, ActionEffect::none, ActionEvidence::none, stamp, stamp};
    if (!valid(fact)) {
        return AYNI_INVALID_ARGUMENT;
    }
    out = ActionRecord(fact);
    return AYNI_OK;
}

bool ActionRecord::advances(ActionStamp stamp) const {
    return valid_stamp(stamp) && stamp.boot_epoch == fact_.latest.boot_epoch &&
           ordered(fact_.latest, stamp);
}

ayni_status_t ActionRecord::begin_dispatch(ActionStamp stamp, uint64_t durable_seq) {
    if (fact_.phase != ActionPhase::prepared || !advances(stamp)) {
        return AYNI_INVALID_STATE;
    }
    if (durable_seq < fact_.prepared.transition.commit_seq) {
        return AYNI_BUSY;
    }
    fact_.phase = ActionPhase::dispatching;
    fact_.effect = ActionEffect::unknown;
    fact_.latest = stamp;
    return AYNI_OK;
}

bool ActionRecord::claim_dispatch(uint64_t current_boot, uint64_t durable_seq) {
    if (fact_.phase != ActionPhase::dispatching || delivery_ != Delivery::unclaimed ||
        current_boot != fact_.latest.boot_epoch ||
        durable_seq < fact_.latest.transition.commit_seq) {
        return false;
    }
    delivery_ = Delivery::claimed;
    return true;
}

ayni_status_t ActionRecord::cancel_before_delivery(ActionStamp stamp) {
    if (delivery_ != Delivery::unclaimed ||
        (fact_.phase != ActionPhase::prepared && fact_.phase != ActionPhase::dispatching)) {
        return AYNI_INVALID_STATE;
    }
    return finish(ActionPhase::cancelled, ActionEffect::none, ActionEvidence::local_non_delivery,
                  stamp);
}

ayni_status_t ActionRecord::rejected_not_retained(ActionStamp stamp) {
    if (fact_.phase != ActionPhase::dispatching || delivery_ != Delivery::claimed) {
        return AYNI_INVALID_STATE;
    }
    return finish(ActionPhase::cancelled, ActionEffect::none, ActionEvidence::local_non_delivery,
                  stamp);
}

bool ActionRecord::receiving() const {
    return delivery_ == Delivery::claimed &&
           (fact_.phase == ActionPhase::dispatching || fact_.phase == ActionPhase::in_progress);
}

ayni_status_t ActionRecord::progress(ActionStamp stamp) {
    if (!receiving() || !advances(stamp)) {
        return AYNI_INVALID_STATE;
    }
    fact_.phase = ActionPhase::in_progress;
    fact_.latest = stamp;
    return AYNI_OK;
}

ayni_status_t ActionRecord::complete(ActionStamp stamp) {
    return receiving() ? finish(ActionPhase::completed, ActionEffect::complete,
                                ActionEvidence::provider_result, stamp)
                       : AYNI_INVALID_STATE;
}

ayni_status_t ActionRecord::fail(ActionEffect effect, ActionStamp stamp) {
    if (!known_effect(effect)) {
        return AYNI_INVALID_ARGUMENT;
    }
    return receiving() ? finish(ActionPhase::failed, effect, ActionEvidence::provider_result, stamp)
                       : AYNI_INVALID_STATE;
}

ayni_status_t ActionRecord::abandon(ActionStamp stamp) {
    return receiving() ? finish(ActionPhase::result_unknown, ActionEffect::unknown,
                                ActionEvidence::none, stamp)
                       : AYNI_INVALID_STATE;
}

// The caller admits an authoritative query, never an absence in a best-effort
// cache. Old delivery must be accounted for before NONE can permit a new binding.
ayni_status_t ActionRecord::reconcile(QueryConclusion conclusion, ActionEffect effect,
                                      OldAttempt disposition, ActionStamp stamp) {
    if (fact_.phase != ActionPhase::result_unknown || delivery_ != Delivery::finished) {
        return AYNI_INVALID_STATE;
    }
    if ((disposition != OldAttempt::drained && disposition != OldAttempt::isolated) ||
        !known_effect(effect)) {
        return AYNI_INVALID_ARGUMENT;
    }
    switch (conclusion) {
    case QueryConclusion::completed:
        return effect == ActionEffect::complete
                   ? finish(ActionPhase::completed, effect, ActionEvidence::authoritative_result,
                            stamp)
                   : AYNI_INVALID_ARGUMENT;
    case QueryConclusion::failed:
        return finish(ActionPhase::failed, effect, ActionEvidence::authoritative_result, stamp);
    case QueryConclusion::not_accepted:
        return effect == ActionEffect::none
                   ? finish(ActionPhase::failed, effect, ActionEvidence::authoritative_not_accepted,
                            stamp)
                   : AYNI_INVALID_ARGUMENT;
    default:
        return AYNI_INVALID_ARGUMENT;
    }
}

ayni_status_t ActionRecord::finish(ActionPhase phase, ActionEffect effect, ActionEvidence evidence,
                                   ActionStamp stamp) {
    if (!advances(stamp)) {
        return AYNI_INVALID_STATE;
    }
    fact_.phase = phase;
    fact_.effect = effect;
    fact_.evidence = evidence;
    fact_.latest = stamp;
    delivery_ = Delivery::finished;
    return AYNI_OK;
}
} // namespace ayni::work
