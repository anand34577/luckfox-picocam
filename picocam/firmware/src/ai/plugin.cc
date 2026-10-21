#include "plugin.h"

#include <dirent.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/config.h"
#include "core/log.h"
#include "core/util.h"

Plugin::~Plugin() {
    if (handle) dlclose(handle);
}

static bool safe_file(const std::string &f) {
    if (f.empty() || f.size() > 64 || !ends_with(f, ".so")) return false;
    for (char c : f)
        if (!isalnum((unsigned char)c) && c != '_' && c != '-' && c != '.') return false;
    return f.find("..") == std::string::npos;
}

std::string plugin_path(const std::string &file) { return safe_file(file) ? g_paths.data + "/plugins/" + file : ""; }

std::shared_ptr<Plugin> load_plugin(const std::string &file, std::string *err) {
    std::string p = plugin_path(file);
    if (p.empty() || !file_exists(p)) {
        if (err) *err = "decoder plugin " + file + " not found (upload it in AI > Models)";
        return nullptr;
    }
    auto pl = std::make_shared<Plugin>();
    pl->handle = dlopen(p.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!pl->handle) {
        if (err) *err = std::string("cannot load plugin: ") + dlerror();
        return nullptr;
    }
    pl->decode = (int (*)(const pc_tensor *, int, int, int, float, pc_det *, int))dlsym(pl->handle, "pc_decode");
    if (!pl->decode) {
        if (err) *err = "plugin has no pc_decode()";
        return nullptr;
    }
    return pl;
}

json plugin_list() {
    json out = json::array();
    DIR *d = opendir((g_paths.data + "/plugins").c_str());
    if (!d) return out;
    while (dirent *e = readdir(d))
        if (safe_file(e->d_name)) out.push_back({{"file", e->d_name}, {"size", file_size(g_paths.data + "/plugins/" + e->d_name)}});
    closedir(d);
    return out;
}

std::string plugin_install(const std::string &filename, const std::string &tmp) {
    std::string base = filename.substr(filename.find_last_of("/\\") + 1), id;
    for (char c : base) id += isalnum((unsigned char)c) || c == '.' || c == '-' ? c : '_';
    if (!safe_file(id)) return "plugin file must end in .so";
    std::string data;
    read_file(tmp, data);
    // an ARM shared object, nothing else
    if (data.size() < 64 || data.compare(0, 4, "\x7f" "ELF") != 0 || (unsigned char)data[18] != 40 || (unsigned char)data[16] != 3)
        return "not an ARM shared library (build it with the camera toolchain)";
    mkdirs(g_paths.data + "/plugins");
    if (!write_file(g_paths.data + "/plugins/" + id, data)) return "cannot store the plugin";
    chmod((g_paths.data + "/plugins/" + id).c_str(), 0700);
    LOGW("plugin", "decoder plugin %s installed", id.c_str());
    return "";
}

std::string plugin_remove(const std::string &file) {
    std::string p = plugin_path(file);
    if (p.empty() || unlink(p.c_str()) != 0) return "no such plugin";
    return "";
}
