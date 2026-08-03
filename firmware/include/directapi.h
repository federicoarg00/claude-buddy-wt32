#pragma once
#include "ui.h"

/* Direct-to-Anthropic mode: the buddy queries the usage endpoint itself with
 * its own OAuth session (provisioned once via provision/provision.js). */

void directapi_init();               /* load persisted tokens from NVS */
bool directapi_has_token();
/* Store a token pair delivered by the provisioning script. plan may be "". */
void directapi_store_tokens(const char *access, const char *refresh,
                            int64_t expiresAtMs, const char *plan);
/* Fetch usage straight from the API. Returns false on any failure.
 * Applies its own 429 backoff: call directapi_wait_ms() for the next delay. */
bool directapi_fetch(BuddyData &out);
uint32_t directapi_wait_ms();
