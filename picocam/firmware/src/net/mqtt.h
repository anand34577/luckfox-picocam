#pragma once
#include <string>

#include "core/config.h"

// MQTT 3.1.1 client (plain TCP or TLS). Publishes events to <topic>/events and a retained online/offline state to
// <topic>/state (last will). Topic may contain {device}. Optional Home Assistant MQTT discovery.
namespace mqtt {

void configure(const json &cfg);  // starts/stops/reconnects as needed
// fire and forget (QoS 0): dropped when the broker is not connected
void publish(const std::string &subtopic, const std::string &payload, bool retain = false);
// reliable (QoS 1): returns "" once the broker acknowledged, else the reason. Blocks up to ~8 s.
std::string publish_wait(const std::string &subtopic, const std::string &payload, bool retain = false);
json status();
void shutdown();

}  // namespace mqtt
