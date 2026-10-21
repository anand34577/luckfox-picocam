#include "models.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <map>
#include <mutex>

#include "core/board.h"
#include "core/log.h"
#include "core/util.h"
#include "plugin.h"
#include "rknn_api.h"

namespace models {

static const char *TAG = "models";
static std::mutex g_mu;

static std::string builtin_dir() { return g_paths.app + "/models"; }
static std::string user_dir() { return g_paths.models(); }

static bool safe_id(const std::string &id) {
    if (id.empty() || id.size() > 96 || !ends_with(id, ".rknn")) return false;
    for (char c : id)
        if (!isalnum((unsigned char)c) && c != '_' && c != '-' && c != '.') return false;
    return id.find("..") == std::string::npos;
}

std::string path(const std::string &id) {
    if (!safe_id(id)) return "";
    for (auto &d : {user_dir(), builtin_dir()})
        if (file_exists(d + "/" + id)) return d + "/" + id;
    return "";
}

static json read_meta(const std::string &file) {
    std::string s;
    std::string side = file.substr(0, file.size() - 5) + ".json";
    if (!read_file(side, s)) return json::object();
    json j = json::parse(s, nullptr, false, true);
    return j.is_object() ? j : json::object();
}

// tensor info for models whose sidecar has none (bundled models), inspected once
static std::map<std::string, json> g_io_cache;
static std::mutex g_io_mu;

static json cached_io(const std::string &file) {
    std::lock_guard<std::mutex> lk(g_io_mu);
    auto it = g_io_cache.find(file);
    if (it == g_io_cache.end()) it = g_io_cache.emplace(file, inspect(file)).first;
    return it->second;
}

json meta(const std::string &id) {
    std::string p = path(id);
    if (p.empty()) return json();
    json m = read_meta(p);
    if (!m.contains("io")) {
        json io = cached_io(p);
        if (!io.contains("error")) m["io"] = io;
    }
    return m;
}

std::string label(const json &m, int cls) {
    if (m.contains("labels") && m["labels"].is_array() && cls >= 0 && cls < (int)m["labels"].size() &&
        m["labels"][cls].is_string())
        return m["labels"][cls].get<std::string>();
    return "class " + std::to_string(cls);
}

json list() {
    std::lock_guard<std::mutex> lk(g_mu);
    json out = json::array();
    std::map<std::string, bool> seen;
    for (auto &d : {user_dir(), builtin_dir()}) {
        DIR *dir = opendir(d.c_str());
        if (!dir) continue;
        while (dirent *e = readdir(dir)) {
            std::string n = e->d_name;
            if (!ends_with(n, ".rknn") || seen[n]) continue;
            seen[n] = true;
            std::string f = d + "/" + n;
            json m = read_meta(f);
            if (!m.contains("io")) {
                json io = cached_io(f);
                if (!io.contains("error")) m["io"] = io;
            }
            out.push_back({{"id", n},
                           {"source", d == user_dir() ? "uploaded" : "bundled"},
                           {"size", file_size(f)},
                           {"name", m.value("name", n)},
                           {"task", m.value("task", "")},
                           {"meta", m}});
        }
        closedir(dir);
    }
    return out;
}

static std::string dims_str(const rknn_tensor_attr &a) {
    std::string s;
    for (uint32_t i = 0; i < a.n_dims; ++i) s += (i ? "x" : "") + std::to_string(a.dims[i]);
    return s;
}

json inspect(const std::string &file) {
    int64_t sz = file_size(file);
    if (sz < 64) return {{"error", "cannot read model"}};
    // a model must fit next to the camera's own memory: 64 MB boards (Pico) take at most 16 MB, 128 MB boards 40 MB
    int64_t cap = meminfo_kb("MemTotal") < 100 * 1024 ? 16LL << 20 : 40LL << 20;
    if (sz > cap) return {{"error", "model too large for this board (" + std::to_string(sz >> 20) + " MB, at most " + std::to_string(cap >> 20) + " MB)"}};
    rknn_context ctx = 0;
    // info-only init from the file path: no NPU memory is touched, safe while the camera's models run, and the model is
    // not copied into RAM by us first
    int ret = rknn_init(&ctx, (void *)file.c_str(), 0, RKNN_FLAG_COLLECT_MODEL_INFO_ONLY, nullptr);
    if (ret < 0) return {{"error", "not a valid RKNN model for this NPU (rknn_init " + std::to_string(ret) + "). This board is an " + board::info().soc +
                          ": models must be converted with --platform " + to_lower(board::info().soc)}};
    rknn_input_output_num io{};
    rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io));
    json ins = json::array(), outs = json::array();
    std::vector<rknn_tensor_attr> oa(io.n_output);
    for (uint32_t i = 0; i < io.n_input; ++i) {
        rknn_tensor_attr a{};
        a.index = i;
        rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &a, sizeof(a));
        ins.push_back({{"name", a.name}, {"dims", dims_str(a)}, {"type", get_type_string(a.type)}, {"fmt", get_format_string(a.fmt)}});
    }
    for (uint32_t i = 0; i < io.n_output; ++i) {
        rknn_tensor_attr &a = oa[i];
        a.index = i;
        rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &a, sizeof(a));
        outs.push_back({{"name", a.name}, {"dims", dims_str(a)}, {"type", get_type_string(a.type)}, {"elems", a.n_elems}});
    }
    rknn_mem_size ms{};
    rknn_query(ctx, RKNN_QUERY_MEM_SIZE, &ms, sizeof(ms));
    rknn_sdk_version v{};
    rknn_query(ctx, RKNN_QUERY_SDK_VERSION, &v, sizeof(v));
    rknn_destroy(ctx);

    // task guess from the output layout
    std::string task = "detect", hint;
    if (io.n_output == 1) {
        uint32_t big = 0;
        for (uint32_t d = 0; d < oa[0].n_dims; ++d) big += oa[0].dims[d] > 1;
        task = big >= 2 ? "ocr" : "classify";
        if (oa[0].n_elems == 128 || oa[0].n_elems == 512) task = "face_embed", hint = "embedding vector";
    } else if (io.n_output == 3) {
        // YOLOv5 heads have 3*(5+classes) channels; RetinaFace has box/score/landmark outputs
        bool retina = false;
        for (auto &a : oa)
            if (a.n_dims >= 2 && (a.dims[a.n_dims - 1] == 10 || a.dims[a.n_dims - 1] == 2)) retina = true;
        task = retina ? "face_detect" : "detect";
    }
    int classes = 0;
    if (task == "detect") {
        if (io.n_output == 3) {
            uint32_t c = oa[0].dims[1] > 3 ? oa[0].dims[1] : oa[0].dims[3];
            classes = (int)c / 3 - 5;
            hint = "YOLOv5 (anchors)";
        } else if (io.n_output == 6 || io.n_output == 9) {
            classes = (int)oa[1].dims[1];
            hint = "YOLOv8 / YOLO11 (DFL)";
        }
    }
    return {{"inputs", ins},       {"outputs", outs},
            {"task_guess", task},  {"classes_guess", classes},
            {"head", hint},        {"weights_kb", ms.total_weight_size / 1024},
            {"internal_kb", ms.total_internal_size / 1024}, {"toolkit", v.api_version}};
}

std::string install(const std::string &filename, const std::string &tmp, const json &m, std::string *id_out) {
    std::string base = filename.substr(filename.find_last_of("/\\") + 1);
    std::string id;
    for (char c : base) id += isalnum((unsigned char)c) || c == '.' || c == '-' ? c : '_';
    if (!ends_with(id, ".rknn")) return "model file must have the .rknn extension";
    if (!safe_id(id)) return "invalid file name";
    json info = inspect(tmp);
    if (info.contains("error")) return info["error"].get<std::string>();
    std::lock_guard<std::mutex> lk(g_mu);
    mkdirs(user_dir());
    std::string dst = user_dir() + "/" + id;
    std::string cmd = "mv -f '" + tmp + "' '" + dst + "'";
    int st = -1;
    run_cmd(cmd, &st);
    if (st != 0) return "cannot store model (disk full?)";
    json meta = m.is_object() ? m : json::object();
    if (!meta.contains("name") || meta["name"].get<std::string>().empty()) meta["name"] = id.substr(0, id.size() - 5);
    if (!meta.contains("task") || meta["task"].get<std::string>().empty()) meta["task"] = info["task_guess"];
    if (!meta.contains("color")) meta["color"] = "rgb";
    meta["io"] = info;
    meta["uploaded"] = wall_ms();
    write_file(dst.substr(0, dst.size() - 5) + ".json", meta.dump(2));
    LOGI(TAG, "installed model %s (%s)", id.c_str(), meta["task"].get<std::string>().c_str());
    *id_out = id;
    return "";
}

std::string install_package(const std::string &tmp, bool allow_plugin, std::string *id_out) {
    std::string dir = tmp + ".pkg";
    remove_tree(dir);
    mkdirs(dir);
    int st = -1;
    std::string out = run_cmd("gunzip -c '" + tmp + "' | tar -x -C '" + dir + "' 2>&1", &st);
    if (st != 0) {
        remove_tree(dir);
        return "not a valid model package: " + trim(out);
    }
    std::string rknn, plugin_file;
    json manifest = json::object();
    std::vector<std::string> labels;
    // flat or one level deep
    std::vector<std::string> names;
    if (DIR *d = opendir(dir.c_str())) {
        while (dirent *e = readdir(d))
            if (e->d_name[0] != '.') names.push_back(e->d_name);
        closedir(d);
    }
    std::string root = dir;
    if (names.size() == 1 && file_size(dir + "/" + names[0]) < 0) {  // a single folder
        root = dir + "/" + names[0];
        names.clear();
        if (DIR *d = opendir(root.c_str())) {
            while (dirent *e = readdir(d))
                if (e->d_name[0] != '.') names.push_back(e->d_name);
            closedir(d);
        }
    }
    for (auto &n : names) {
        if (ends_with(n, ".rknn")) {
            if (!rknn.empty()) {
                remove_tree(dir);
                return "the package holds more than one .rknn model";
            }
            rknn = n;
        } else if (ends_with(n, ".so")) {
            plugin_file = n;
        } else if (n == "manifest.json") {
            std::string s;
            read_file(root + "/" + n, s);
            json j = json::parse(s, nullptr, false, true);
            if (j.is_object()) manifest = j;
        } else if (n == "labels.txt") {
            std::string s;
            read_file(root + "/" + n, s);
            for (auto &l : split(s, '\n'))
                if (!trim(l).empty()) labels.push_back(trim(l));
        }
    }
    if (rknn.empty()) {
        remove_tree(dir);
        return "no .rknn file in the package";
    }
    if (!plugin_file.empty()) {
        if (!allow_plugin) {
            remove_tree(dir);
            return "this package contains a decoder plugin: only an administrator may install it";
        }
        std::string e = plugin_install(plugin_file, root + "/" + plugin_file);
        if (!e.empty()) {
            remove_tree(dir);
            return e;
        }
        manifest["decoder"] = "plugin";
        manifest["plugin"] = plugin_file;
    }
    if (!labels.empty() && !manifest.contains("labels")) manifest["labels"] = labels;
    manifest.erase("io");
    manifest.erase("uploaded");
    std::string err = install(rknn, root + "/" + rknn, manifest, id_out);
    remove_tree(dir);
    return err;
}

std::string update_meta(const std::string &id, const json &m) {
    std::string p = path(id);
    if (p.empty()) return "unknown model";
    std::lock_guard<std::mutex> lk(g_mu);
    json cur = read_meta(p);
    for (auto it = m.begin(); it != m.end(); ++it)
        if (it.key() != "io") cur[it.key()] = *it;
    // bundled models are read-only: keep an override sidecar next to uploads
    std::string side = starts_with(p, user_dir()) ? p.substr(0, p.size() - 5) + ".json" : "";
    if (side.empty()) {
        mkdirs(user_dir());
        // copy the model into the user dir so the override takes precedence
        int st = -1;
        run_cmd("cp -f '" + p + "' '" + user_dir() + "/" + id + "'", &st);
        if (st != 0) return "cannot store model";
        side = user_dir() + "/" + id.substr(0, id.size() - 5) + ".json";
    }
    write_file(side, cur.dump(2));
    return "";
}

std::string remove(const std::string &id) {
    if (!safe_id(id)) return "invalid model id";
    std::lock_guard<std::mutex> lk(g_mu);
    std::string f = user_dir() + "/" + id;
    if (!file_exists(f)) return file_exists(builtin_dir() + "/" + id) ? "bundled models cannot be deleted" : "unknown model";
    unlink(f.c_str());
    unlink((f.substr(0, f.size() - 5) + ".json").c_str());
    return "";
}

}  // namespace models
