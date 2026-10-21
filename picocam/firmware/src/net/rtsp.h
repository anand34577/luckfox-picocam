#pragma once
#include <string>

// RTSP server (RFC 2326) for the main and sub streams.
//   rtsp://<ip>[:554]/live/main   /live/sub   (also /live/0 /live/1, /stream1 /stream2,
//   /Streaming/Channels/101 and /102, /h264/ch1/main/av_stream and .../sub/av_stream, /cam/realmonitor?subtype=0|1)
// Transport: RTP over TCP (interleaved) or UDP unicast. Auth: Digest (default), Basic or none.
// Viewers that fall behind skip frames until the next keyframe instead of building up delay.
namespace rtsp {

bool start(int port);
void stop();
void configure(const std::string &auth_mode);  // "digest" | "basic" | "none"
int sessions();
// drop all sessions (encoder settings changed)
void kick_all();

}  // namespace rtsp
