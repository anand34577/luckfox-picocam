#pragma once
#include <string>

#include "core/config.h"

// Delivery of events to the outside world, built to survive bad networks.
//
// Every stored event is queued separately for each matching target (webhooks, MQTT, EdgeAI server, FTP, SMTP, Telegram).
// A queue item is a small file in the data directory, so nothing is lost on reboot; per-target workers retry with
// exponential backoff, so one dead endpoint never delays the others. Pictures are read from the event store at delivery
// time. Local actions (GPIO pulse for a gate relay / lamp) run immediately.
namespace delivery {

void start();
void stop();
// called for every stored event; `rec` is the index record (+ optional "embedding")
void enqueue(const json &rec);
json status();  // per-target state and queue sizes
// validates the "integrations" section ("" or a message)
std::string validate(json &integrations);
void apply(const json &now);  // settings changed

}  // namespace delivery
