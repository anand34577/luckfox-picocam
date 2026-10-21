#pragma once
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "core/config.h"

// Event store and delivery. Producers (AI rules, motion) call emit(); a worker thread takes the full-resolution
// snapshot, stores the event (SD card when present), pushes it to the web UI and delivers it to the configured
// integrations (webhooks, MQTT, EdgeAI server).
namespace events {

struct Event {
    std::string type;  // intrusion, line_cross, loitering, count, presence, track, motion, ...
    std::string rule_id, rule_name;
    std::string label;  // object class
    int track_id = 0;
    float score = 0;
    json box;                                               // [x1,y1,x2,y2] normalised to the picture
    json attributes = json::object();                       // e.g. {"plate": "MH12AB1234", "vehicle_type": "car"}
    std::vector<std::pair<std::string, std::string>> crops;  // name -> JPEG bytes
    bool snapshot = true;
    json extra = json::object();
};

void start();
void stop();
void emit(Event e);  // thread-safe, never blocks (drops when the queue is full)
// called (on the events thread) for every stored event; used by ONVIF event subscriptions
void add_listener(std::function<void(const json &rec)> fn);
json status();
void register_routes();
// where events and their pictures live (SD card when mounted, else internal flash)
std::string dir();

}  // namespace events
