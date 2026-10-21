#pragma once
#include <memory>
#include <string>

#include "core/config.h"
#include "pc_plugin.h"

// A decoder plugin (shared library, see pc_plugin.h) loaded with dlopen.
struct Plugin {
    void *handle = nullptr;
    int (*decode)(const pc_tensor *, int, int, int, float, pc_det *, int) = nullptr;
    ~Plugin();
};

// <data>/plugins/<file>.so
std::string plugin_path(const std::string &file);
std::shared_ptr<Plugin> load_plugin(const std::string &file, std::string *err);
// list / install / remove uploaded plugins (administrators only at the API)
json plugin_list();
std::string plugin_install(const std::string &filename, const std::string &tmp_path);
std::string plugin_remove(const std::string &file);
