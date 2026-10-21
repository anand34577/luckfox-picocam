#pragma once
#include <string>

#include "config.h"

// Security audit trail (logins, configuration changes, user and firmware actions) and a small persistent log of
// warnings/errors. Both live in the data directory so they survive reboots; each file is capped and rotated.
namespace audit {

void init();  // after g_paths is known
// who: user name (or "-"), peer: client address
void record(const std::string &who, const std::string &peer, const std::string &action, const std::string &detail = "");
json tail(size_t max);  // newest first
// persistent WARN/ERROR log (survives reboot); newest last
std::string persistent_log_tail(size_t max_bytes);
void persist_log_line(const std::string &line);  // called by the logger

}  // namespace audit
