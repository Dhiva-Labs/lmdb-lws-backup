#pragma once

#include <string>

namespace lwsbk {

// POSTs a small JSON failure event to the webhook (via the system curl
// binary, stdin-fed, 15s timeout). Never throws — alerting failures are
// logged and swallowed; they must not mask the original backup error.
// No-op when url is empty. The payload contains only stage/error text and
// hostname — key material never reaches this function by construction.
void fire_failure_webhook(const std::string& url, const std::string& stage,
                          const std::string& error_msg);

}  // namespace lwsbk
