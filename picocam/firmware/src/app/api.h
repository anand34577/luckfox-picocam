#pragma once

// REST API (/api/...). Feature modules register their own routes; this file holds the core ones:
// session/login, status, settings, snapshot, users/keys and system maintenance.
namespace api {
void register_routes();
}
