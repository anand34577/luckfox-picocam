#pragma once
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "json.hpp"

using json = nlohmann::json;

// Where things live on the board.
struct Paths {
    std::string app = "/oem/picocam";        // binary, web UI, bundled models (read-mostly)
    std::string data = "/userdata/picocam";  // config, users, uploaded models, event index
    std::string www() const { return app + "/www"; }
    std::string config() const { return data + "/config.json"; }
    std::string models() const { return data + "/models"; }
    std::string events() const { return data + "/events"; }
};
extern Paths g_paths;

// The device configuration: built-in defaults overlaid with config.json. Sections are top-level keys
// ("video", "image", "ai", ...). Modules register a validator/applier per section; an update is merged,
// validated, applied, then persisted. Readers get copies, so they never see a half-applied change.
class Settings {
public:
    // validator returns "" when the merged section is acceptable, else a message for the UI.
    // applier is called (outside the lock) with the new and old section after a successful update.
    using Validator = std::function<std::string(json &section)>;
    using Applier = std::function<void(const json &now, const json &before)>;

    bool load();
    json get(const std::string &section) const;
    json all() const;
    // deep-merge `patch` into the section (objects merge, arrays/scalars replace)
    std::string update(const std::string &section, const json &patch, bool persist = true);
    // replace a whole section (e.g. rules list)
    std::string replace(const std::string &section, const json &value, bool persist = true);
    void on(const std::string &section, Validator v, Applier a);
    // store a value without validation or appliers (used to roll back a change that could not be applied)
    void revert(const std::string &section, const json &value);
    bool save();
    void factory_reset(bool keep_network);
    json defaults() const;

private:
    std::string commit(const std::string &section, json next, bool persist);
    mutable std::mutex mu_;
    json cfg_;
    std::map<std::string, std::vector<std::pair<Validator, Applier>>> hooks_;
};

extern Settings g_settings;

// small helpers for reading json with defaults
template <typename T>
T jget(const json &j, const char *key, T def) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return def;
    try {
        return it->get<T>();
    } catch (...) {
        return def;
    }
}
