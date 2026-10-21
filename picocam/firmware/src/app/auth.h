#pragma once
#include <string>

#include "core/config.h"

// Users, sessions and API keys.
//   roles: admin (everything), operator (live, playback, settings except users/system), viewer (live, playback)
// Like commercial cameras the device starts "inactive": the first visit to the web UI sets the admin
// password. Until then RTSP/ONVIF refuse access.
namespace auth {

enum Role { NONE = 0, VIEWER = 1, OPERATOR = 2, ADMIN = 3 };
Role role_from(const std::string &s);
const char *role_name(Role r);

void load();
bool activated();
std::string activate(const std::string &password);  // "" on success

// passwords
Role check_password(const std::string &user, const std::string &pass);
// RTSP/HTTP Digest: HA1 = MD5(user:realm:pass)
std::string ha1(const std::string &user);
const char *realm();
// plain password (needed for ONVIF WS-UsernameToken digests)
std::string secret(const std::string &user);

// sessions (web UI)
std::string create_session(const std::string &user, Role r, const std::string &peer = "");
// signed-in sessions (ids are a hash of the token, safe to show); revoke by id
json list_sessions(const std::string &current_token);
bool revoke_session(const std::string &id);
int revoke_other_sessions(const std::string &keep_token);
std::string session_id(const std::string &token);
Role check_session(const std::string &token, std::string *user = nullptr);
void drop_session(const std::string &token);

// API keys (integrations): "pk_" + 32 hex, stored hashed
Role check_api_key(const std::string &key, std::string *name = nullptr);

// user management (admin)
json list_users();
std::string set_user(const std::string &name, const std::string &password, const std::string &role);
std::string delete_user(const std::string &name);
json list_keys();
std::string create_key(const std::string &name, const std::string &role, std::string *key_out);
std::string delete_key(const std::string &name);

// brute-force protection: true while `peer` is locked out
bool locked_out(const std::string &peer);
void note_failure(const std::string &peer);
void note_success(const std::string &peer);

}  // namespace auth
