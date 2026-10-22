/* Since ABI 0.16: trusted Host ownership and authenticated session entry.
 *
 * These functions are for the composition root/Host after authenticating the
 * owner and channel. Applications receive only session references. This is
 * an API capability boundary, not isolation from trusted native code in the
 * same process (ADR-0003, ADR-0028). Credentials remain outside Core.
 */
#ifndef AYNI_HOST_AUTHORITY_H
#define AYNI_HOST_AUTHORITY_H

#include <ayni/ayni.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Registers the owner principal, whom the host has bound and authenticated.
 * Returns AYNI_OK; AYNI_INVALID_ARGUMENT for a NULL pointer, a kind other
 * than AYNI_PRINCIPAL_OWNER, a zero principal_id or binding_revision, or a
 * nonzero reserved field; AYNI_INVALID_ARGUMENT or AYNI_UNSUPPORTED_VERSION
 * for struct_size; AYNI_INVALID_STATE outside CONFIGURING;
 * AYNI_CAPACITY_EXCEEDED once an owner is registered, since a device has
 * exactly one (C-PROD-001).
 */
ayni_status_t ayni_register_principal(ayni_kernel_t *kernel,
                                      const ayni_principal_desc_v1 *principal);

/*
 * Opens a session for a principal on a channel the host has authenticated
 * (C-AUTH-010, ADR-0028). Sessions are not stored: a reboot closes them all. On
 * AYNI_OK, *out_session_id receives a new id, never 0. Returns AYNI_OK;
 * AYNI_INVALID_ARGUMENT for a NULL pointer, unknown channel, zero binding
 * revision or nonzero reserved field;
 * AYNI_INVALID_ARGUMENT or AYNI_UNSUPPORTED_VERSION for struct_size;
 * AYNI_INVALID_STATE when called from a callback; AYNI_NOT_READY outside
 * RUNNING or until the owner binding is durable; AYNI_NOT_FOUND for an
 * unregistered principal; AYNI_DENIED for a binding revision mismatch;
 * AYNI_CAPACITY_EXCEEDED once session_capacity sessions are open or no id is
 * left this boot (C-ID-010).
 */
ayni_status_t ayni_open_session(ayni_kernel_t *kernel, const ayni_session_desc_v1 *session,
                                uint64_t *out_session_id);

/* Since 0.19. Owner-thread lookup after Host authentication of the
 * principal. Never trust a network-supplied identity. Accepted revocations
 * remain queryable after automatic reload closes the old Session. Pending
 * identity precedes the ordinary command window/floor. NOT_FOUND/EXPIRED
 * leave output untouched; CONFIGURING/STOPPED return INVALID_STATE. */
ayni_status_t ayni_get_intent_receipt(const ayni_kernel_t *kernel,
                                      uint64_t authenticated_principal_id, uint64_t command_id,
                                      ayni_intent_receipt_v1 *out_receipt);

#ifdef __cplusplus
}
#endif

#endif /* AYNI_HOST_AUTHORITY_H */
