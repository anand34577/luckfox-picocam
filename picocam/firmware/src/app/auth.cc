#include "auth.h"

#include <map>
#include <mutex>

#include "core/log.h"
#include "core/util.h"

namespace auth {

static const char *TAG = "auth";
static const char *kRealm = "PicoCam";

static std::mutex g_mu;
static json g_db;  // {"users": [...], "keys": [...]}

struct Session {
    std::string user;
    Role role;
    int64_t expires;
    std::string peer;
    int64_t created = 0;
};
static std::map<std::string, Session> g_sessions;

struct Fail {
    int count = 0;
    int64_t until = 0;
};
static std::map<std::string, Fail> g_fails;

static std::string db_path() { return g_paths.data + "/users.json"; }

// The plain password is kept (ONVIF digests need it) but not in clear text: XOR with a per-device key.
static std::string device_key() {
    std::string k;
    if (!read_file(g_paths.data + "/.key", k) || k.size() != 32) {
        k = random_bytes(32);
        mkdirs(g_paths.data);
        write_file(g_paths.data + "/.key", k);
    }
    return k;
}
static std::string obfuscate(const std::string &s) {
    std::string k = device_key(), o = s;
    for (size_t i = 0; i < o.size(); ++i) o[i] ^= k[i % k.size()];
    return base64_encode(o);
}
static std::string deobfuscate(const std::string &s) {
    std::string k = device_key(), o = base64_decode(s);
    for (size_t i = 0; i < o.size(); ++i) o[i] ^= k[i % k.size()];
    return o;
}

Role role_from(const std::string &s) {
    if (s == "admin") return ADMIN;
    if (s == "operator") return OPERATOR;
    if (s == "viewer") return VIEWER;
    return NONE;
}

const char *role_name(Role r) {
    switch (r) {
        case ADMIN: return "admin";
        case OPERATOR: return "operator";
        case VIEWER: return "viewer";
        default: return "none";
    }
}

const char *realm() { return kRealm; }

static void save_locked() { write_file(db_path(), g_db.dump(2)); }

void load() {
    std::lock_guard<std::mutex> lk(g_mu);
    std::string s;
    g_db = json::object();
    if (read_file(db_path(), s)) {
        try {
            g_db = json::parse(s);
        } catch (...) {
            LOGE(TAG, "users.json unreadable");
        }
    }
    if (!g_db.contains("users")) g_db["users"] = json::array();
    if (!g_db.contains("keys")) g_db["keys"] = json::array();
}

bool activated() {
    std::lock_guard<std::mutex> lk(g_mu);
    return !g_db["users"].empty();
}

static std::string pw_problem(const std::string &p) {
    if (p.size() < 8) return "password must be at least 8 characters";
    if (p.size() > 64) return "password too long";
    for (char c : p)
        if ((unsigned char)c < 32 || c == 127) return "password may not contain control characters";
    bool alpha = false, digit = false;
    for (char c : p) alpha |= isalpha((unsigned char)c) != 0, digit |= isdigit((unsigned char)c) != 0;
    if (!alpha || !digit) return "password must contain letters and digits";
    return "";
}

static json make_user(const std::string &name, const std::string &pass, const std::string &role) {
    std::string salt = random_token(8);
    return {{"name", name},
            {"role", role},
            {"salt", salt},
            {"hash", sha256_hex(salt + pass)},
            {"ha1", md5_hex(name + ":" + kRealm + ":" + pass)},
            {"secret", obfuscate(pass)},
            {"created", wall_ms()}};
}

std::string activate(const std::string &password) {
    std::string e = pw_problem(password);
    if (!e.empty()) return e;
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_db["users"].empty()) return "already activated";
    g_db["users"].push_back(make_user("admin", password, "admin"));
    save_locked();
    LOGI(TAG, "device activated");
    return "";
}

static json *find_user(const std::string &name) {
    for (auto &u : g_db["users"])
        if (u.value("name", "") == name) return &u;
    return nullptr;
}

Role check_password(const std::string &user, const std::string &pass) {
    std::lock_guard<std::mutex> lk(g_mu);
    json *u = find_user(user);
    if (!u) return NONE;
    std::string h = sha256_hex(u->value("salt", "") + pass);
    return const_time_eq(h, u->value("hash", "")) ? role_from(u->value("role", "")) : NONE;
}

std::string ha1(const std::string &user) {
    std::lock_guard<std::mutex> lk(g_mu);
    json *u = find_user(user);
    return u ? u->value("ha1", "") : "";
}

std::string secret(const std::string &user) {
    std::lock_guard<std::mutex> lk(g_mu);
    json *u = find_user(user);
    return u ? deobfuscate(u->value("secret", "")) : "";
}

std::string session_id(const std::string &token) { return sha256_hex(token).substr(0, 12); }

json list_sessions(const std::string &current_token) {
    std::lock_guard<std::mutex> lk(g_mu);
    json out = json::array();
    int64_t now = wall_ms();
    for (auto &kv : g_sessions)
        if (kv.second.expires >= now)
            out.push_back({{"id", session_id(kv.first)}, {"user", kv.second.user}, {"role", role_name(kv.second.role)},
                           {"peer", kv.second.peer}, {"created", kv.second.created}, {"expires", kv.second.expires},
                           {"current", kv.first == current_token}});
    return out;
}

bool revoke_session(const std::string &id) {
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto it = g_sessions.begin(); it != g_sessions.end(); ++it)
        if (session_id(it->first) == id) {
            g_sessions.erase(it);
            return true;
        }
    return false;
}

int revoke_other_sessions(const std::string &keep_token) {
    std::lock_guard<std::mutex> lk(g_mu);
    int n = 0;
    for (auto it = g_sessions.begin(); it != g_sessions.end();)
        if (it->first != keep_token) it = g_sessions.erase(it), ++n;
        else ++it;
    return n;
}

std::string create_session(const std::string &user, Role r, const std::string &peer) {
    int hours = jget<int>(g_settings.get("web"), "session_hours", 12);
    std::string t = random_token(24);
    std::lock_guard<std::mutex> lk(g_mu);
    // expire old sessions
    int64_t now = wall_ms();
    for (auto it = g_sessions.begin(); it != g_sessions.end();) it = it->second.expires < now ? g_sessions.erase(it) : ++it;
    g_sessions[t] = {user, r, now + (int64_t)hours * 3600 * 1000, peer, now};
    return t;
}

Role check_session(const std::string &token, std::string *user) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_sessions.find(token);
    if (it == g_sessions.end() || it->second.expires < wall_ms()) return NONE;
    // the user may have been deleted or downgraded since login
    json *u = find_user(it->second.user);
    if (!u) return NONE;
    if (user) *user = it->second.user;
    return role_from(u->value("role", ""));
}

void drop_session(const std::string &token) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_sessions.erase(token);
}

Role check_api_key(const std::string &key, std::string *name) {
    if (!starts_with(key, "pk_")) return NONE;
    std::string h = sha256_hex(key);
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto &k : g_db["keys"])
        if (const_time_eq(k.value("hash", ""), h)) {
            if (name) *name = k.value("name", "");
            return role_from(k.value("role", "viewer"));
        }
    return NONE;
}

json list_users() {
    std::lock_guard<std::mutex> lk(g_mu);
    json out = json::array();
    for (auto &u : g_db["users"]) out.push_back({{"name", u["name"]}, {"role", u["role"]}, {"created", u.value("created", 0)}});
    return out;
}

static int admin_count() {
    int n = 0;
    for (auto &u : g_db["users"]) n += u.value("role", "") == "admin";
    return n;
}

std::string set_user(const std::string &name, const std::string &password, const std::string &role) {
    if (name.empty() || name.size() > 32) return "invalid user name";
    for (char c : name)
        if (!isalnum((unsigned char)c) && c != '_' && c != '-' && c != '.') return "user name may use letters, digits, _ - .";
    if (role_from(role) == NONE) return "role must be admin, operator or viewer";
    std::lock_guard<std::mutex> lk(g_mu);
    json *u = find_user(name);
    if (u && role != "admin" && u->value("role", "") == "admin" && admin_count() <= 1) return "cannot demote the last admin";
    if (password.empty()) {
        if (!u) return "password required";
        (*u)["role"] = role;
    } else {
        std::string e = pw_problem(password);
        if (!e.empty()) return e;
        json nu = make_user(name, password, role);
        if (u) {
            nu["created"] = u->value("created", wall_ms());
            *u = nu;
        } else {
            if (g_db["users"].size() >= 16) return "too many users";
            g_db["users"].push_back(nu);
        }
    }
    save_locked();
    return "";
}

std::string delete_user(const std::string &name) {
    std::lock_guard<std::mutex> lk(g_mu);
    json &arr = g_db["users"];
    for (size_t i = 0; i < arr.size(); ++i)
        if (arr[i].value("name", "") == name) {
            if (arr[i].value("role", "") == "admin" && admin_count() <= 1) return "cannot delete the last admin";
            arr.erase(i);
            save_locked();
            for (auto it = g_sessions.begin(); it != g_sessions.end();)
                it = it->second.user == name ? g_sessions.erase(it) : ++it;
            return "";
        }
    return "no such user";
}

json list_keys() {
    std::lock_guard<std::mutex> lk(g_mu);
    json out = json::array();
    for (auto &k : g_db["keys"])
        out.push_back({{"name", k["name"]}, {"role", k["role"]}, {"prefix", k.value("prefix", "")}, {"created", k.value("created", 0)}});
    return out;
}

std::string create_key(const std::string &name, const std::string &role, std::string *key_out) {
    if (name.empty() || name.size() > 40) return "invalid key name";
    if (role_from(role) == NONE) return "invalid role";
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto &k : g_db["keys"])
        if (k.value("name", "") == name) return "a key with this name exists";
    std::string key = "pk_" + random_token(16);
    g_db["keys"].push_back(
        {{"name", name}, {"role", role}, {"hash", sha256_hex(key)}, {"prefix", key.substr(0, 7)}, {"created", wall_ms()}});
    save_locked();
    *key_out = key;
    return "";
}

std::string delete_key(const std::string &name) {
    std::lock_guard<std::mutex> lk(g_mu);
    json &arr = g_db["keys"];
    for (size_t i = 0; i < arr.size(); ++i)
        if (arr[i].value("name", "") == name) {
            arr.erase(i);
            save_locked();
            return "";
        }
    return "no such key";
}

bool locked_out(const std::string &peer) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto it = g_fails.find(peer);
    return it != g_fails.end() && it->second.until > now_ms();
}

void note_failure(const std::string &peer) {
    std::lock_guard<std::mutex> lk(g_mu);
    Fail &f = g_fails[peer];
    if (++f.count >= 5) {
        // 5 wrong passwords: 1 minute, doubling up to 30 minutes
        int mins = std::min(30, 1 << std::min(5, f.count - 5));
        f.until = now_ms() + mins * 60000;
        LOGW(TAG, "too many failed logins from %s, locked for %d min", peer.c_str(), mins);
    }
}

void note_success(const std::string &peer) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_fails.erase(peer);
}

}  // namespace auth
