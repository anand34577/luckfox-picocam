#include "config.h"

#include "log.h"
#include "util.h"

Paths g_paths;
Settings g_settings;

static const char *TAG = "config";

// Factory defaults. Anything missing from config.json falls back to these, so new firmware versions can add
// settings without migrations.
static const char *kDefaults = R"JSON({
  "device": { "name": "PicoCam", "location": "", "id": "" },

  "video": {
    "sensor_fps": 25,
    "main": { "enabled": true, "codec": "h264", "width": 2304, "height": 1296, "fps": 25,
              "rc": "cbr", "bitrate": 3072, "max_bitrate": 4096, "gop": 50, "profile": "high",
              "quality": "high", "smart_p": false },
    "sub":  { "enabled": true, "codec": "h264", "width": 960, "height": 540, "fps": 15,
              "rc": "cbr", "bitrate": 1024, "max_bitrate": 1536, "gop": 30, "profile": "main",
              "quality": "high", "smart_p": false, "ai_overlay": false },
    "jpeg": { "quality": 80 }
  },

  "image": {
    "brightness": 50, "contrast": 50, "saturation": 50, "sharpness": 50, "hue": 50,
    "mirror": false, "flip": false,
    "exposure": { "mode": "smart", "preset": "general", "preset_applied": "general",
                  "target": 50, "compensation": 0, "anti_flicker": "auto",
                  "max_shutter_ms": 40, "max_gain": 64, "shutter_ms": 10, "gain": 1,
                  "metering": "center", "speed": "normal", "strategy": "lowlight",
                  "ai_roi": false, "motion_shutter": false, "blur_px": 3,
                  "tuning": { "gain": 0.4, "deadband": 0.2, "backlight_hi": 0.03, "backlight_lo": 0.18 } },
    "wdr": { "mode": "auto", "level": 50 },
    "backlight": "off",
    "dehaze": { "mode": "off", "level": 50 },
    "nr": { "spatial": 50, "temporal": 50 },
    "wb": { "mode": "auto", "r": 50, "g": 50, "b": 50 },
    "daynight": { "mode": "color", "night_lv": 0, "day_lv": 0, "ircut_gpio": -1, "irled_gpio": -1, "ircut_invert": false }
  },

  "osd": {
    "time": { "enabled": true, "position": "top-left", "format": "%Y-%m-%d %H:%M:%S" },
    "name": { "enabled": true, "position": "bottom-right", "text": "" },
    "size": "medium"
  },

  "masks": [],

  "rtsp": { "enabled": true, "port": 554, "auth": "digest", "multicast": false },
  "onvif": { "enabled": true, "discovery": true },
  "web": { "port": 80, "session_hours": 12, "https": { "enabled": true, "port": 443, "redirect": false } },

  "network": { "hostname": "picocam", "mode": "dhcp", "ip": "", "netmask": "255.255.255.0",
               "gateway": "", "dns": ["8.8.8.8", "1.1.1.1"],
               "usb_ip": "172.32.0.93", "usb_netmask": "255.255.255.0" },
  "time": { "timezone": "IST-5:30", "tz_name": "Asia/Kolkata", "ntp": true,
            "ntp_server": "pool.ntp.org", "ntp_interval_min": 60 },

  "ai": {
    "enabled": false,
    "max_fps": 10,
    "detector": { "model": "", "conf": 0.4, "nms": 0.45, "classes": [], "min_box_px": 12 },
    "stages": [],
    "draw_on_web": true,
    "track": { "iou": 0.25, "max_misses": 10 }
  },

  "rules": [],

  "motion": { "enabled": false, "sensitivity": 60, "min_area_pct": 1.0, "cooldown_s": 10,
              "zones": [] },

  "events": { "snapshot": true, "snapshot_source": "main", "snapshot_quality": 80, "keep": 2000, "store_on_sd": true, "internal_flash": false },

  "integrations": {
    "webhooks": [],
    "mqtt": { "enabled": false, "host": "", "port": 1883, "username": "", "password": "",
              "client_id": "", "topic": "picocam/{device}", "retain_status": true, "include_image": false,
              "tls": false, "verify": "system", "pin": "", "ha_discovery": false },
    "edgeai_server": { "enabled": false, "url": "", "heartbeat_s": 10, "verify": "system", "pin": "" },
    "ftp": { "enabled": false, "host": "", "port": 21, "user": "", "password": "", "path": "/picocam", "what": "snapshot",
             "types": [], "rules": [], "labels": [] },
    "smtp": { "enabled": false, "host": "", "port": 587, "security": "starttls", "user": "", "password": "", "from": "", "to": "",
              "subject": "PicoCam alert", "include_image": true, "min_interval_s": 30, "verify": "system", "pin": "",
              "types": [], "rules": [], "labels": [] },
    "telegram": { "enabled": false, "token": "", "chat_id": "", "include_image": true, "min_interval_s": 5,
                  "types": [], "rules": [], "labels": [] },
    "gpio": []
  },

  "recording": { "mode": "off", "stream": "main", "segment_min": 5, "pre_s": 5, "post_s": 15,
                 "schedule": [], "overwrite": true, "reserve_mb": 512 },

  "storage": { "path": "auto" },

  "security": { "allow": [] },

  "system": { "watchdog": true, "ssh": true, "adb": true, "root_sync": true }
})JSON";

json Settings::defaults() const { return json::parse(kDefaults); }

// Objects merge recursively; anything else replaces.
static void merge(json &dst, const json &src) {
    if (!dst.is_object() || !src.is_object()) {
        dst = src;
        return;
    }
    for (auto it = src.begin(); it != src.end(); ++it) {
        auto d = dst.find(it.key());
        if (d != dst.end() && d->is_object() && it->is_object()) merge(*d, *it);
        else dst[it.key()] = *it;
    }
}

bool Settings::load() {
    json cfg = defaults();
    std::string text;
    if (read_file(g_paths.config(), text)) {
        try {
            merge(cfg, json::parse(text, nullptr, true, true));
        } catch (const std::exception &e) {
            LOGE(TAG, "config.json unreadable (%s), using defaults; bad file kept as config.json.bad", e.what());
            rename(g_paths.config().c_str(), (g_paths.config() + ".bad").c_str());
        }
    }
    if (jget<std::string>(cfg["device"], "id", "").empty()) {
        // stable id from the board's MAC (falls back to random, then persisted)
        std::string mac = iface_mac("eth0");
        if (mac.empty()) mac = iface_mac("usb0");
        std::string id = mac.empty() ? random_token(6) : mac;
        id.erase(std::remove(id.begin(), id.end(), ':'), id.end());
        cfg["device"]["id"] = "picocam-" + id.substr(id.size() > 6 ? id.size() - 6 : 0);
    }
    std::lock_guard<std::mutex> lk(mu_);
    cfg_ = cfg;
    return true;
}

json Settings::get(const std::string &section) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = cfg_.find(section);
    return it == cfg_.end() ? json() : *it;
}

json Settings::all() const {
    std::lock_guard<std::mutex> lk(mu_);
    return cfg_;
}

void Settings::on(const std::string &section, Validator v, Applier a) {
    std::lock_guard<std::mutex> lk(mu_);
    hooks_[section].push_back({v, a});
}

std::string Settings::update(const std::string &section, const json &patch, bool persist) {
    json next = get(section);
    if (next.is_null()) return "unknown section " + section;
    merge(next, patch);
    return commit(section, next, persist);
}

std::string Settings::replace(const std::string &section, const json &value, bool persist) {
    if (get(section).is_null()) return "unknown section " + section;
    return commit(section, value, persist);
}

std::string Settings::commit(const std::string &section, json next, bool persist) {
    std::vector<std::pair<Validator, Applier>> hooks;
    json before;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = hooks_.find(section);
        if (it != hooks_.end()) hooks = it->second;
        before = cfg_[section];
    }
    for (auto &h : hooks)
        if (h.first) {
            std::string err = h.first(next);
            if (!err.empty()) return err;
        }
    {
        std::lock_guard<std::mutex> lk(mu_);
        cfg_[section] = next;
    }
    if (persist) save();  // persist=false: live preview (sliders), flash is written once the user stops
    for (auto &h : hooks)
        if (h.second && next != before) h.second(next, before);
    return "";
}

void Settings::revert(const std::string &section, const json &value) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        cfg_[section] = value;
    }
    save();
}

bool Settings::save() {
    std::string text;
    {
        std::lock_guard<std::mutex> lk(mu_);
        text = cfg_.dump(2);
    }
    mkdirs(g_paths.data);
    if (!write_file(g_paths.config(), text)) {
        LOGE(TAG, "cannot write %s", g_paths.config().c_str());
        return false;
    }
    return true;
}

void Settings::factory_reset(bool keep_network) {
    json d = defaults();
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (keep_network) d["network"] = cfg_["network"];
        d["device"]["id"] = cfg_["device"]["id"];
        cfg_ = d;
    }
    save();
}
