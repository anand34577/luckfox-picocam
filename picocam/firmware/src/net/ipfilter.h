#pragma once
#include <string>

#include "core/config.h"

// Optional allow-list of client addresses for every TCP service (web, RTSP, ONVIF). Empty = everyone.
namespace ipfilter {
std::string validate(const json &allow_list);  // "" or a message for the UI
void configure(const json &allow_list);
bool allowed(const std::string &ip);
}  // namespace ipfilter
