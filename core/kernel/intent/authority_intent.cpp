#include "kernel/intent/authority_intent.h"

namespace ayni::kernel {
namespace {
ayni_status_t refuse(ayni_status_t status, ayni_reason_t reason, IntentFailure &failure,
                     ayni_object_ref_v1 object = {}) {
    failure = {reason, object};
    return status;
}
} // namespace

ayni_status_t create_grant(WorkModel &model, const Intent &intent,
                           const authority::Session &session, uint64_t now_us, uint64_t durable_seq,
                           IntentOutcome &outcome, IntentFailure &failure) {
    if (session.channel != AYNI_CHANNEL_MANAGEMENT) {
        return refuse(AYNI_DENIED, AYNI_REASON_GRANT_CHANNEL, failure);
    }
    const authority::OwnerBinding *binding = model.principals.binding();
    if (binding == nullptr || binding->principal_id != session.principal_id ||
        binding->binding_revision != session.binding_revision ||
        binding->stamp.commit_seq > durable_seq) {
        return refuse(AYNI_DENIED, AYNI_REASON_GRANT_CHANNEL, failure);
    }
    const ayni_grant_create_v1 &body = intent.grant_create;
    if ((body.flags & (AYNI_GRANT_REQUIRES_SCENE | AYNI_GRANT_REQUIRES_QUOTA)) != 0U) {
        return refuse(AYNI_UNSUPPORTED, AYNI_REASON_GRANT_UNSUPPORTED, failure);
    }
    const auto capability = model.capabilities.find(body.capability_id, body.capability_version);
    switch (capability.kind) {
    case catalog::CapabilityRegistry::Lookup::Kind::unknown_id:
        return refuse(AYNI_NOT_FOUND, AYNI_REASON_CAPABILITY_UNKNOWN, failure);
    case catalog::CapabilityRegistry::Lookup::Kind::unknown_version:
        return refuse(AYNI_UNSUPPORTED_VERSION, AYNI_REASON_CAPABILITY_VERSION, failure);
    case catalog::CapabilityRegistry::Lookup::Kind::found:
        break;
    }
    const work::AgentInstance *subject = model.agents.find_instance(body.subject_instance_id);
    const ayni_object_ref_v1 subject_ref{0, body.subject_instance_id,
                                         subject == nullptr ? 0U : subject->revision,
                                         AYNI_OBJECT_AGENT_INSTANCE};
    if (subject == nullptr ||
        work::AgentTable::effective_lifecycle(*subject) == AYNI_AGENT_STOPPED ||
        work::AgentTable::effective_lifecycle(*subject) == AYNI_AGENT_FAULTED) {
        return refuse(AYNI_DENIED, AYNI_REASON_GRANT_SUBJECT, failure, subject_ref);
    }
    const authority::NewGrant grant{body.subject_instance_id,
                                    body.parent_grant_id,
                                    session.principal_id,
                                    session.binding_revision,
                                    model.capabilities.at(capability.index).id,
                                    body.capability_version,
                                    model.capabilities.domain_view(capability.index),
                                    &body.scope,
                                    body.lifetime,
                                    body.duration_us,
                                    body.flags};
    authority::GrantTable::Admission admission{};
    const auto check = model.grants.check_create(
        grant, intent.expected_revision, {model.ids.boot_epoch(), now_us, durable_seq}, admission);
    if (check.status != AYNI_OK) {
        return refuse(check.status, check.reason, failure);
    }
    if (body.parent_grant_id != 0U) {
        uint32_t parent = 0;
        if (!model.grants.find(body.parent_grant_id, parent)) {
            return AYNI_INTERNAL;
        }
        const auto chain = model.check_grant(parent, now_us, durable_seq);
        if (chain.status != AYNI_OK) {
            return refuse(chain.status, chain.reason, failure);
        }
    }
    if (!model.ids.available()) {
        return refuse(AYNI_CAPACITY_EXCEEDED, AYNI_REASON_ID_EXHAUSTED, failure);
    }
    uint64_t id = 0;
    static_cast<void>(model.ids.allocate(id));
    const auto stamp = model.transitions.next();
    uint32_t index = 0;
    if (!model.grants.insert(grant, admission, id, stamp, index)) {
        return AYNI_INTERNAL;
    }
    outcome.object = {0, id, model.grants.at(index).revision, AYNI_OBJECT_GRANT};
    outcome.commit_seq = stamp.commit_seq;
    model.due_work_changed = true;
    outcome.due_work = true;
    return AYNI_OK;
}
void pending_revoke_outcome(const PendingRevoke &pending, const CommandMapping *mapping,
                            IntentOutcome &outcome) {
    if (pending.queued_commit_seq == 0U) {
        outcome.object = {0, pending.accepted.target_grant_id, 0, AYNI_OBJECT_GRANT};
        outcome.phase = AYNI_INTENT_PENDING;
        outcome.effect = AYNI_INTENT_EFFECT_NONE;
        outcome.commit_seq = 0;
        outcome.applied_boot_epoch = 0;
        return;
    }
    if (mapping != nullptr) {
        outcome.object = mapping->object;
        outcome.commit_seq = mapping->commit_seq;
        outcome.effect = mapping->effect;
        outcome.phase = AYNI_INTENT_APPLIED;
        outcome.applied_boot_epoch = pending.queued_boot_epoch;
    }
}

ayni_status_t accept_revoke(WorkModel &model, const Intent &intent,
                            const authority::Session &session, uint64_t durable_seq,
                            IntentOutcome &outcome, IntentFailure &failure) {
    const auto *binding = model.principals.binding();
    if (session.channel != AYNI_CHANNEL_MANAGEMENT || binding == nullptr ||
        binding->principal_id != session.principal_id ||
        binding->binding_revision != session.binding_revision ||
        binding->stamp.commit_seq > durable_seq) {
        return refuse(AYNI_DENIED, AYNI_REASON_GRANT_CHANNEL, failure);
    }
    const auto check = model.grants.check_revoke(intent.target_id, intent.expected_revision);
    if (check.status != AYNI_OK) {
        return refuse(check.status, check.reason, failure);
    }
    const AcceptedRevoke accepted{session.principal_id, intent.command_id, session.binding_revision,
                                  intent.expected_revision, intent.target_id};
    const auto capacity = model.commands.can_accept_revoke(accepted);
    if (capacity != AYNI_OK) {
        return refuse(capacity,
                      capacity == AYNI_BUSY ? AYNI_REASON_REVOCATION_PENDING
                                            : AYNI_REASON_REVOCATION_CAPACITY,
                      failure);
    }
    if (!model.grants.can_gate(intent.target_id)) {
        return refuse(AYNI_CAPACITY_EXCEEDED, AYNI_REASON_REVOCATION_CAPACITY, failure);
    }
    // No callbacks or other owner work intervene between these reservations.
    const PendingRevoke *pending = nullptr;
    if (!model.grants.install_gate(intent.target_id) ||
        model.commands.accept_revoke(accepted, pending) != AYNI_OK || pending == nullptr) {
        return AYNI_INTERNAL;
    }
    pending_revoke_outcome(*pending, nullptr, outcome);
    outcome.due_work = true;
    return AYNI_OK;
}

bool apply_pending_revoke(WorkModel &model) {
    const auto *pending = model.commands.next_pending_revoke();
    if (pending == nullptr || !model.commands.can_publish_accepted(*pending)) {
        return false;
    }
    const auto plan = model.grants.plan_accepted_revoke(pending->accepted.target_grant_id);
    const auto stamp = model.transitions.next();
    const CommandMapping mapping{pending->accepted.principal_id,
                                 pending->accepted.command_id,
                                 0,
                                 stamp.commit_seq,
                                 AYNI_INTENT_GRANT_REVOKE,
                                 {0, plan.grant_id, plan.revision, AYNI_OBJECT_GRANT},
                                 0,
                                 plan.effect};
    if (!model.grants.apply_revoke(plan, stamp) ||
        !model.commands.publish_accepted_revoke(*pending, mapping, model.ids.boot_epoch())) {
        return false;
    }
    model.due_work_changed = true;
    return true;
}

bool apply_revocation_cleanup(WorkModel &model) {
    authority::GrantTable::CleanupPlan plan{};
    if (!model.grants.next_revocation_cleanup(plan)) {
        return false;
    }
    return model.grants.apply_cleanup(plan, model.transitions.next());
}
} // namespace ayni::kernel
