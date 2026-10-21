#pragma once
#include <string>

#include "core/config.h"

namespace sys {

void start();  // stats sampler, NTP client, applies time/network settings
void stop();

json stats();        // cpu, memory, media memory, temperature, uptime, load
json device_info();  // model, ids, versions, interfaces
json network_status();

std::string validate_network(json &n);
void apply_network(const json &n);  // hostname, USB address, Ethernet DHCP/static
void apply_usb(const json &n);
std::string validate_time(json &t);
void apply_time(const json &t);
bool set_clock(int64_t epoch_ms);
void ntp_sync_now();

void reboot(int delay_ms = 1500);

// Media memory (CMA) size used by the ISP, encoders and NPU. Stored in the U-Boot environment
// (rk_dma_heap_cma=NNM); takes effect after a reboot. Only SD-booted boards expose the env partition.
json media_memory();
std::string set_media_memory(int mb);
// ---- self-healing and hardening (selfheal.cc)
void loop_beat();  // the event loop calls this every second: proof that it is alive
// Hardware watchdog (/dev/watchdog): fed only while the loop and the video path look healthy, so a hang resets the
// board instead of leaving it dead on a pole. Disarmed on a clean shutdown.
void watchdog_apply(bool on);
void watchdog_shutdown();
std::string validate_system(json &s);
void apply_system(const json &s, const json &before);  // watchdog, SSH/ADB switches
// The root account follows the admin password (one password for the web UI and the shell).
bool sync_root_password(const std::string &password);
json security_status();
// A network change made over the network can strand the camera: the previous settings come back after 60 s unless
// the change is confirmed from the new address.
void network_changed(const json &now, const json &before);
void network_confirm();
int network_pending_s();  // 0 = nothing to confirm
// firmware state: previous version kept, rolled back / config recovered by the supervisor
json update_state();
std::string rollback_update();  // "" on success; the caller restarts the service

const char *version();
std::string board_model();  // "Luckfox Pico", "Luckfox Pico Plus", ...

}  // namespace sys
