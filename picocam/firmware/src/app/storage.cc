#include "storage.h"

#include <dirent.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#include "core/board.h"
#include "app/system.h"
#include "core/log.h"
#include "core/util.h"
#include "media/mp4.h"
#include "media/stream.h"
#include "net/http.h"

namespace storage {

static const char *TAG = "storage";
static const size_t kQueueBytes = 3 * 1024 * 1024;
static const size_t kPreBytes = 2560 * 1024;

static std::mutex g_mu;  // root/kind/state
static std::string g_root, g_kind = "none", g_dev, g_fs, g_err;
static std::atomic<bool> g_run{false};
static std::thread g_th;
static std::atomic<int64_t> g_event_until{0};
static std::atomic<bool> g_ejected{false};  // user pressed "safely remove": stay unmounted until the card is swapped

// packets from the encoder
static std::mutex g_qmu;
static std::condition_variable g_qcv;
static std::deque<Packet> g_q;
static size_t g_qbytes = 0;
static int g_sub = 0;
static Stream *g_stream = nullptr;

// recorder state (recorder thread, read by status)
static std::string g_cur_file;
static std::atomic<int64_t> g_cur_bytes{0};
static std::atomic<bool> g_recording{false};
static std::atomic<int64_t> g_dropped{0};

// ---------------------------------------------------------------- detection / mounting

static bool boot_from_sd() {
    std::string c;
    read_file("/proc/cmdline", c);
    return c.find("storagemedia=sd") != std::string::npos;
}

static std::string mounted_dev(const std::string &mp, std::string *fs = nullptr) {
    std::string m;
    read_file("/proc/mounts", m);
    for (auto &l : split(m, '\n')) {
        auto f = split(l, ' ');
        if (f.size() >= 3 && f[1] == mp) {
            if (fs) *fs = f[2];
            return f[0];
        }
    }
    return "";
}

static std::mutex g_det_mu;  // the recorder thread and the API both call detect()

// the "media" partition PicoCam adds after the system partition on SD-booted boards (sys::card_layout), or ""
static std::string media_partition() {
    DIR *dir = opendir("/sys/class/block");
    if (!dir) return "";
    std::string found;
    while (dirent *e = readdir(dir)) {
        std::string ue;
        if (strncmp(e->d_name, "mmcblk", 6) != 0 || !read_file(std::string("/sys/class/block/") + e->d_name + "/uevent", ue)) continue;
        if (ue.find("PARTNAME=media\n") != std::string::npos) found = std::string("/dev/") + e->d_name;
    }
    closedir(dir);
    return found;
}

static bool is_ext4(const std::string &dev) {
    uint8_t sb[2];
    FILE *fp = fopen(dev.c_str(), "rb");
    if (!fp) return false;
    bool ok = fseek(fp, 1024 + 56, SEEK_SET) == 0 && fread(sb, 1, 2, fp) == 2;  // superblock magic 0xef53
    fclose(fp);
    return ok && sb[0] == 0x53 && sb[1] == 0xef;
}

static bool mkfs_media(const std::string &dev) {
    int st = -1;
    // one inode per 256 KB: plenty for segments and event pictures, and a quicker format and check
    std::string out = run_cmd("mkfs.ext4 -F -q -L media -m 0 -i 262144 -E lazy_itable_init=1 " + dev + " 2>&1", &st);
    if (st != 0) LOGE(TAG, "formatting %s failed: %s", dev.c_str(), trim(out).c_str());
    return st == 0;
}

// checks (a power cut can leave the journal to replay) and mounts the media partition at /mnt/sdcard; formats it the
// first time. Returns "" or what went wrong.
static std::string mount_media(const std::string &d) {
    std::string cur = mounted_dev("/mnt/sdcard");
    if (cur == d) return "";
    if (!cur.empty()) run_cmd("umount /mnt/sdcard 2>/dev/null || umount -l /mnt/sdcard");
    mkdirs("/mnt/sdcard");
    int st = -1;
    if (!is_ext4(d)) {
        LOGW(TAG, "%s: new media partition, formatting it (ext4)", d.c_str());
        if (!mkfs_media(d)) return "the media partition could not be formatted";
    } else {
        run_cmd("e2fsck -p " + d + " >/dev/null 2>&1", &st);
        if (st >= 4) {
            LOGW(TAG, "%s: file system errors after a power cut, repairing", d.c_str());
            run_cmd("e2fsck -fy " + d + " >/dev/null 2>&1", &st);
        }
    }
    run_cmd("mount -t ext4 -o noatime " + d + " /mnt/sdcard 2>&1", &st);
    if (st != 0) {
        // a camera that records again beats one that keeps a broken file system: start the partition afresh
        LOGE(TAG, "%s cannot be mounted even after repair: formatting it, older recordings are lost", d.c_str());
        if (!mkfs_media(d)) return "the media partition is damaged and could not be formatted";
        run_cmd("mount -t ext4 -o noatime " + d + " /mnt/sdcard 2>&1", &st);
        if (st != 0) return "the media partition cannot be mounted";
    }
    LOGI(TAG, "media partition %s mounted at /mnt/sdcard", d.c_str());
    return "";
}

static void detect() {
    std::lock_guard<std::mutex> dl(g_det_mu);
    std::string root, kind = "none", dev, fs;
    std::string media = boot_from_sd() ? media_partition() : "";
    if (!media.empty()) {
        // recordings on their own partition of the system card: the system partition can stay read-only
        kind = "media";
        dev = media;
        static int64_t failed_ms = 0;  // a card that cannot be mounted is retried every 5 minutes, not on every packet
        std::string err = failed_ms && now_ms() - failed_ms < 300000 ? g_err : mount_media(media);
        failed_ms = err.empty() ? 0 : failed_ms ? failed_ms : now_ms();
        if (!err.empty()) {
            std::lock_guard<std::mutex> lk(g_mu);
            g_kind = kind, g_dev = dev, g_root.clear(), g_err = err;
            return;
        }
        mounted_dev("/mnt/sdcard", &fs);
        root = "/mnt/sdcard";
    } else if (boot_from_sd()) {
        // the SD card is the system disk; its big root partition holds the recordings
        // (/mnt/sdcard is only the small userdata partition on these images)
        kind = "boot_sd";
        root = "/";
        dev = mounted_dev(root, &fs);
        if (dev == "/dev/root") {
            std::string c;
            read_file("/proc/cmdline", c);
            size_t p = c.find("root=");
            if (p != std::string::npos) dev = split(c.substr(p + 5), ' ')[0];
        }
    } else {
        // the slot is mmcblk1 on boards with eMMC and often mmcblk0 on NAND boards: ask sysfs, never assume
        std::string d = board::find_sd_device();
        if (d.empty()) {
            g_ejected = false;  // card physically removed: the next one mounts normally
            // a pulled card leaves a dead mount behind; a re-inserted card would never be mounted over it
            if (!mounted_dev("/mnt/sdcard").empty()) {
                run_cmd("umount -l /mnt/sdcard 2>/dev/null");
                LOGW(TAG, "SD card removed: dropped the stale mount");
            }
        } else if (g_ejected) {
            std::lock_guard<std::mutex> lk(g_mu);
            g_kind = "sd_ejected";
            g_dev = d;
            g_root.clear();
            g_err = "";
            return;
        } else {
            kind = "sd";
            dev = d;
            if (mounted_dev("/mnt/sdcard", &fs).empty()) {
                mkdirs("/mnt/sdcard");
                // repair a card that lost power mid-write before mounting (FAT only; no-op when the tool is missing)
                run_cmd("command -v fsck.fat >/dev/null 2>&1 && fsck.fat -a " + d + " >/dev/null 2>&1; true");
                int st = -1;
                run_cmd("mount -o noatime " + d + " /mnt/sdcard 2>&1", &st);
                if (st != 0) run_cmd("mount " + d + " /mnt/sdcard 2>&1", &st);
                if (st != 0) {
                    std::lock_guard<std::mutex> lk(g_mu);
                    g_kind = "sd_unmounted";
                    g_dev = d;
                    g_root.clear();
                    g_err = "SD card present but not readable (unsupported file system such as exFAT? Format it from Storage)";
                    return;
                }
                mounted_dev("/mnt/sdcard", &fs);
                LOGI(TAG, "mounted %s (%s) at /mnt/sdcard", d.c_str(), fs.c_str());
            }
            root = "/mnt/sdcard";
        }
    }
    std::lock_guard<std::mutex> lk(g_mu);
    if (root != g_root) LOGI(TAG, "storage: %s", root.empty() ? "none" : (root + " (" + kind + ")").c_str());
    g_root = root;
    g_kind = kind;
    g_dev = dev;
    g_fs = fs;
    if (!root.empty()) g_err.clear();
}

std::string root() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_root.empty()) return "";
    return g_root == "/" ? "/picocam" : g_root + "/picocam";
}

static void space(const std::string &path, int64_t &total, int64_t &free) {
    struct statvfs s;
    total = free = 0;
    if (statvfs(path.c_str(), &s) == 0) {
        total = (int64_t)s.f_blocks * s.f_frsize;
        free = (int64_t)s.f_bavail * s.f_frsize;
    }
}

// ---------------------------------------------------------------- files

static std::vector<std::string> list_dir(const std::string &d) {
    std::vector<std::string> v;
    DIR *dir = opendir(d.c_str());
    if (!dir) return v;
    while (dirent *e = readdir(dir))
        if (e->d_name[0] != '.') v.push_back(e->d_name);
    closedir(dir);
    std::sort(v.begin(), v.end());
    return v;
}

static std::string rec_dir() { return root() + "/rec"; }

// free space by deleting the oldest recordings
static bool make_room(int64_t reserve, bool overwrite) {
    std::string r = root();
    int64_t total, free;
    space(r, total, free);
    if (free >= reserve) return true;
    if (!overwrite) return false;
    for (int guard = 0; guard < 200 && free < reserve; ++guard) {
        auto days = list_dir(rec_dir());
        if (days.empty()) break;
        std::string dd = rec_dir() + "/" + days[0];
        auto files = list_dir(dd);
        if (files.empty()) {
            rmdir(dd.c_str());
            continue;
        }
        {
            std::lock_guard<std::mutex> lk(g_mu);
            if (dd + "/" + files[0] == g_cur_file) break;
        }
        unlink((dd + "/" + files[0]).c_str());
        LOGI(TAG, "overwrite: removed %s/%s", days[0].c_str(), files[0].c_str());
        space(r, total, free);
    }
    return free >= reserve;
}

// ---------------------------------------------------------------- recorder

static bool scheduled(const json &rec) {
    json s = rec.value("schedule", json::array());
    if (!s.is_array() || s.size() != 168) return true;  // no grid = always
    time_t t = time(nullptr);
    struct tm tm;
    localtime_r(&t, &tm);
    return s[tm.tm_wday * 24 + tm.tm_hour].get<int>() != 0;
}

static void subscribe(int stream) {
    if (g_stream && g_sub) g_stream->unsubscribe(g_sub);
    g_stream = stream_by_index(stream);
    g_sub = g_stream->subscribe([](const Packet &p) {
        std::lock_guard<std::mutex> lk(g_qmu);
        if (g_qbytes + p.data->size() > kQueueBytes) {  // storage too slow: drop
            ++g_dropped;
            return;
        }
        g_qbytes += p.data->size();
        g_q.push_back(p);
        g_qcv.notify_one();
    });
}

static void loop() {
    prctl(PR_SET_NAME, "recorder", 0, 0, 0);
    Mp4Writer w;
    std::deque<Packet> pre;  // pre-event buffer, starts at a keyframe
    size_t pre_bytes = 0;
    int64_t seg_start = 0, gen = -1, last_detect = 0;
    std::string part, final_base, day;
    bool event_seg = false;
    int cur_stream = -1;

    auto close_seg = [&]() {
        if (!w.is_open()) return;
        w.close();
        std::string fin = final_base;
        fin.insert(fin.find('_'), "-" + format_time(time(nullptr), "%H%M%S"));
        rename(part.c_str(), fin.c_str());
        LOGI(TAG, "saved %s (%lld KB)", fin.substr(fin.find("/rec/") + 5).c_str(), (long long)(w.bytes() / 1024));
        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_cur_file.clear();
        }
        g_recording = false;
    };

    while (g_run) {
        if (now_ms() - last_detect > 10000) {
            detect();
            last_detect = now_ms();
        }
        json rec = g_settings.get("recording");
        std::string mode = jget<std::string>(rec, "mode", "off");
        int stream = jget<std::string>(rec, "stream", "main") == "sub" ? 1 : 0;
        if (stream != cur_stream) {
            close_seg();
            subscribe(stream);
            cur_stream = stream;
            pre.clear();
            pre_bytes = 0;
        }
        Packet p;
        {
            std::unique_lock<std::mutex> lk(g_qmu);
            g_qcv.wait_for(lk, std::chrono::milliseconds(500), [] { return !g_q.empty() || !g_run; });
            if (g_q.empty()) {
                if (mode == "off") close_seg();
                continue;
            }
            p = g_q.front();
            g_q.pop_front();
            g_qbytes -= p.data->size();
        }
        std::string r = root();
        bool want = false;
        if (!r.empty()) {
            if (mode == "continuous") want = true;
            else if (mode == "schedule") want = scheduled(rec);
            else if (mode == "events") want = now_ms() < g_event_until;
        }
        // pre-event buffer (events mode)
        if (mode == "events" && !w.is_open()) {
            if (p.key) {
                // keep whole GOPs; trim old ones past pre_s or the memory cap
                int64_t keep_us = std::max(1, jget<int>(rec, "pre_s", 5)) * 1000000LL;
                while (!pre.empty() && (p.pts_us - pre.front().pts_us > keep_us || pre_bytes > kPreBytes)) {
                    // drop up to the next keyframe
                    do {
                        pre_bytes -= pre.front().data->size();
                        pre.pop_front();
                    } while (!pre.empty() && !pre.front().key);
                }
            }
            if (p.key || !pre.empty()) {
                pre.push_back(p);
                pre_bytes += p.data->size();
            }
        }
        if (!want) {
            if (w.is_open() && (mode != "events" || p.key)) close_seg();
            continue;
        }
        int64_t seg_us = std::max(1, std::min(60, jget<int>(rec, "segment_min", 5))) * 60000000LL;
        bool rotate = w.is_open() && p.key &&
                      (p.pts_us - seg_start > seg_us || w.bytes() > 1000LL * 1024 * 1024 ||  // FAT cannot hold files over 4 GB
                       g_stream->generation() != gen ||
                       format_time(time(nullptr), "%Y-%m-%d") != day);
        if (rotate) close_seg();
        if (!w.is_open()) {
            std::deque<Packet> backlog;
            if (mode == "events" && !pre.empty()) backlog.swap(pre), pre_bytes = 0;
            const Packet &first = backlog.empty() ? p : backlog.front();
            if (!first.key) continue;
            auto ps = g_stream->params();
            if (!ps.valid()) continue;
            if (!make_room(std::max(64, jget<int>(rec, "reserve_mb", 512)) * 1024LL * 1024, jget<bool>(rec, "overwrite", true))) {
                std::lock_guard<std::mutex> lk(g_mu);
                g_err = "storage full (overwrite disabled)";
                continue;
            }
            day = format_time(time(nullptr), "%Y-%m-%d");
            std::string dir = rec_dir() + "/" + day;
            mkdirs(dir);
            event_seg = mode == "events";
            final_base = dir + "/" + format_time(time(nullptr) - (int64_t)(backlog.empty() ? 0 : (p.pts_us - first.pts_us) / 1000000), "%H%M%S") +
                         "_" + (cur_stream ? "sub" : "main") + (event_seg ? "_e" : "") + ".mp4";
            part = final_base + ".part";
            if (!w.open(part, g_stream->codec(), g_stream->width(), g_stream->height(), ps)) {
                std::lock_guard<std::mutex> lk(g_mu);
                g_err = "cannot write to storage";
                continue;
            }
            {
                std::lock_guard<std::mutex> lk(g_mu);
                g_err.clear();
            }
            gen = g_stream->generation();
            seg_start = first.pts_us;
            {
                std::lock_guard<std::mutex> lk(g_mu);
                g_cur_file = part;
            }
            g_recording = true;
            for (auto &b : backlog)
                if (&b != &backlog.back() || b.pts_us != p.pts_us) w.write(b);
        }
        if (!w.write(p)) {
            LOGE(TAG, "write failed, closing segment");
            close_seg();
        }
        g_cur_bytes = w.bytes();
    }
    close_seg();
    if (g_stream && g_sub) g_stream->unsubscribe(g_sub);
}

void trigger(const std::string &reason) {
    json rec = g_settings.get("recording");
    int64_t until = now_ms() + std::max(1, jget<int>(rec, "post_s", 15)) * 1000LL;
    if (until > g_event_until) g_event_until = until;
}

void start() {
    detect();
    g_run = true;
    g_th = std::thread(loop);
}

void stop() {
    g_run = false;
    g_qcv.notify_all();
    if (g_th.joinable()) g_th.join();
}

json status() {
    std::string r = root();
    int64_t total = 0, free = 0;
    if (!r.empty()) {
        mkdirs(r);
        space(r, total, free);
    }
    std::lock_guard<std::mutex> lk(g_mu);
    json rec = g_settings.get("recording");
    return {{"kind", g_kind},
            {"device", g_dev},
            {"fs", g_fs},
            {"path", r},
            {"available", !r.empty()},
            {"total_bytes", total},
            {"free_bytes", free},
            {"error", g_err},
            {"recording", g_recording.load()},
            {"mode", jget<std::string>(rec, "mode", "off")},
            {"current_file", g_cur_file.empty() ? "" : g_cur_file.substr(g_cur_file.find_last_of('/') + 1)},
            {"current_bytes", g_cur_bytes.load()},
            {"dropped_frames", g_dropped.load()},
            {"can_format", g_kind == "sd" || g_kind == "sd_unmounted" || g_kind == "sd_ejected" || g_kind == "media"},
            {"can_eject", g_kind == "sd"},
            {"card", g_kind == "boot_sd" || g_kind == "media" ? sys::card_layout() : json()},
            {"root_mode", sys::root_mode()}};
}

// ---------------------------------------------------------------- API

static bool safe_name(const std::string &s) {
    if (s.empty() || s.size() > 64 || s.find("..") != std::string::npos) return false;
    for (char c : s)
        if (!isalnum((unsigned char)c) && c != '-' && c != '_' && c != '.') return false;
    return true;
}

// serve a file with HTTP Range support (seeking in the browser player)
void serve_range(HttpRequest &req, HttpResponse &res, const std::string &path, const std::string &type) {
    int64_t size = file_size(path);
    if (size < 0) return res.error(404, "not found");
    res.headers["Content-Type"] = type;
    res.headers["Accept-Ranges"] = "bytes";
    res.headers["Cache-Control"] = "private, max-age=60";
    std::string range = req.header("range");
    int64_t a = 0, b = size - 1;
    if (starts_with(range, "bytes=")) {
        std::string r = range.substr(6);
        size_t dash = r.find('-');
        if (dash != std::string::npos) {
            std::string s1 = r.substr(0, dash), s2 = r.substr(dash + 1);
            if (s1.empty()) {
                a = std::max<int64_t>(0, size - atoll(s2.c_str()));
            } else {
                a = atoll(s1.c_str());
                if (!s2.empty()) b = std::min<int64_t>(size - 1, atoll(s2.c_str()));
            }
        }
        if (a > b || a >= size) {
            res.headers["Content-Range"] = "bytes */" + std::to_string(size);
            return res.error(416, "range not satisfiable");
        }
        res.status = 206;
        res.headers["Content-Range"] = "bytes " + std::to_string(a) + "-" + std::to_string(b) + "/" + std::to_string(size);
    }
    res.file = path;
    res.file_off = a;
    res.file_len = b - a + 1;
}

void register_routes() {
    g_settings.on(
        "recording",
        [](json &r) {
            std::string m = jget<std::string>(r, "mode", "off");
            if (m != "off" && m != "continuous" && m != "events" && m != "schedule") return std::string("invalid recording mode");
            if (r.contains("schedule") && !r["schedule"].empty() && (!r["schedule"].is_array() || r["schedule"].size() != 168))
                return std::string("schedule must have 7x24 entries");
            return std::string();
        },
        nullptr);

    g_http.route("GET", "/api/storage", auth::VIEWER, [](HttpRequest &, HttpResponse &res) { res.send_json(status()); });

    g_http.route("POST", "/api/storage/format", auth::ADMIN, [](HttpRequest &req, HttpResponse &res) {
        json b = req.json_body();
        std::string fs = jget<std::string>(b, "fs", "vfat");
        std::string dev, kind;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            dev = g_dev;
            kind = g_kind;
        }
        if (kind != "sd" && kind != "sd_unmounted" && kind != "sd_ejected" && kind != "media")
            return res.error(400, "only a removable SD card or the media partition can be formatted");
        if (kind == "media") fs = "ext4";  // the media partition is always ext4 (journal: power-cut safe)
        g_ejected = false;
        if (fs != "vfat" && fs != "ext4") return res.error(400, "fs must be vfat or ext4");
        g_settings.update("recording", {{"mode", "off"}});
        usleep(800 * 1000);
        run_cmd("umount /mnt/sdcard 2>/dev/null");
        int st = -1;
        if (kind == "media") {
            if (!mkfs_media(dev)) return res.error(500, "format failed (see the log)");
        } else {
            std::string out = run_cmd((fs == "ext4" ? "mkfs.ext4 -F -q -L PICOCAM " : "mkfs.vfat -n PICOCAM ") + dev + " 2>&1", &st);
            if (st != 0) return res.error(500, "format failed: " + trim(out));
        }
        detect();
        LOGW(TAG, "%s formatted as %s by %s", dev.c_str(), fs.c_str(), req.user.c_str());
        res.send_json(status());
    }, true);

    // safe removal: stop writing, flush, unmount; the card stays unmounted until it is swapped or mounted again
    g_http.route("POST", "/api/storage/eject", auth::OPERATOR, [](HttpRequest &req, HttpResponse &res) {
        std::string kind;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            kind = g_kind;
        }
        if (kind != "sd") return res.error(400, "no removable SD card is mounted");
        g_ejected = true;
        detect();  // clears the root: the recorder closes its file on the next packet
        for (int i = 0; i < 40 && g_recording; ++i) usleep(100 * 1000);
        sync();
        int st = -1;
        std::string out = run_cmd("umount /mnt/sdcard 2>&1", &st);
        if (st != 0) run_cmd("umount -l /mnt/sdcard 2>&1");
        LOGI(TAG, "SD card ejected by %s%s", req.user.c_str(), st != 0 ? " (lazy)" : "");
        res.send_json(status());
    }, true);
    g_http.route("POST", "/api/storage/mount", auth::OPERATOR, [](HttpRequest &, HttpResponse &res) {
        g_ejected = false;
        detect();
        res.send_json(status());
    }, true);

    g_http.route("GET", "/api/recordings/days", auth::VIEWER, [](HttpRequest &, HttpResponse &res) {
        json out = json::array();
        if (!root().empty())
            for (auto &d : list_dir(rec_dir())) out.push_back(d);
        res.send_json(out);
    });

    g_http.route("GET", "/api/recordings", auth::VIEWER, [](HttpRequest &req, HttpResponse &res) {
        std::string day = req.param("date", format_time(time(nullptr), "%Y-%m-%d"));
        if (!safe_name(day) || root().empty()) return res.send_json(json::array());
        json out = json::array();
        std::string cur;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            cur = g_cur_file;
        }
        for (auto &f : list_dir(rec_dir() + "/" + day)) {
            // HHMMSS-HHMMSS_stream[_e].mp4  or  HHMMSS_stream[_e].mp4.part (in progress / interrupted)
            bool part = ends_with(f, ".part");
            std::string start = f.substr(0, 6), end = f.size() > 13 && f[6] == '-' ? f.substr(7, 6) : "";
            out.push_back({{"name", f},
                           {"start", start},
                           {"end", end},
                           {"in_progress", part && rec_dir() + "/" + day + "/" + f == cur},
                           {"event", f.find("_e.") != std::string::npos},
                           {"stream", f.find("_sub") != std::string::npos ? "sub" : "main"},
                           {"size", file_size(rec_dir() + "/" + day + "/" + f)}});
        }
        res.send_json(out);
    });

    g_http.route("GET", "/api/recordings/file/", auth::VIEWER, [](HttpRequest &req, HttpResponse &res) {
        if (req.path_args.size() != 2 || !safe_name(req.path_args[0]) || !safe_name(req.path_args[1]))
            return res.error(400, "bad path");
        std::string p = rec_dir() + "/" + req.path_args[0] + "/" + req.path_args[1];
        serve_range(req, res, p, "video/mp4");
        if (req.param("download") == "1")
            res.headers["Content-Disposition"] = "attachment; filename=\"" + req.path_args[0] + "_" + req.path_args[1] + "\"";
    });

    g_http.route("DELETE", "/api/recordings/file/", auth::OPERATOR, [](HttpRequest &req, HttpResponse &res) {
        if (req.path_args.size() != 2 || !safe_name(req.path_args[0]) || !safe_name(req.path_args[1]))
            return res.error(400, "bad path");
        std::string p = rec_dir() + "/" + req.path_args[0] + "/" + req.path_args[1];
        {
            std::lock_guard<std::mutex> lk(g_mu);
            if (p == g_cur_file) return res.error(409, "file is being recorded");
        }
        if (unlink(p.c_str()) != 0) return res.error(404, "not found");
        res.ok();
    });
}

}  // namespace storage
