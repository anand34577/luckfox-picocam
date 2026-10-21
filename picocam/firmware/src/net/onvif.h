#pragma once

// ONVIF Profile S (device, media, events, imaging) + WS-Discovery, so NVRs and VMS software can find the camera,
// pull the streams and receive motion/AI alarms.
//   service endpoints: http://<ip>/onvif/device_service, /onvif/media_service, /onvif/events_service,
//   /onvif/imaging_service
namespace onvif {

void start();  // routes + discovery (if enabled in settings "onvif")
void stop();

}  // namespace onvif
