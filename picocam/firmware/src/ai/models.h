#pragma once
#include <string>

#include "core/config.h"

// Model library: .rknn files with a sidecar <file>.json describing them.
//   bundled: <app>/models (read-only, shipped with the firmware)   uploaded: <data>/models
// Sidecar fields: name, task (detect | classify | ocr | face_detect | face_embed), labels[], color (rgb|bgr),
// charset (ocr), description, plus "io" (tensor shapes, filled in on upload).
namespace models {

json list();
std::string path(const std::string &id);  // "" if unknown
json meta(const std::string &id);
// install an uploaded file; returns "" or an error. *id_out = stored file name
std::string install(const std::string &filename, const std::string &tmp_path, const json &meta, std::string *id_out);
// A model package: a .tar.gz holding one .rknn, optionally manifest.json (any model sidecar field: name, task, labels,
// decoder, anchors, ...), labels.txt (one per line) and a decoder plugin (.so, needs `allow_plugin`). One file carries
// everything the model needs; models/package.py builds them.
std::string install_package(const std::string &tmp_path, bool allow_plugin, std::string *id_out);
std::string update_meta(const std::string &id, const json &meta);
std::string remove(const std::string &id);
// tensor shapes and a task guess, without allocating NPU memory
json inspect(const std::string &file);
std::string label(const json &meta, int cls);

}  // namespace models
