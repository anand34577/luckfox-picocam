#include "onvif.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

#include "app/auth.h"
#include "app/events.h"
#include "app/system.h"
#include "core/log.h"
#include "core/util.h"
#include "http.h"
#include "media/stream.h"

namespace onvif {

static const char *TAG = "onvif";

// ---------------------------------------------------------------- helpers

static std::string ip() { return iface_ipv4(primary_iface()); }
static int http_port() { return jget<int>(g_settings.get("web"), "port", 80); }
static int rtsp_port() { return jget<int>(g_settings.get("rtsp"), "port", 554); }
static std::string base_url() {
    int p = http_port();
    return "http://" + ip() + (p == 80 ? "" : ":" + std::to_string(p));
}

// stable across reboots (the USB gadget MAC is random per boot): derived from the persisted device id
static std::string device_uuid() {
    std::string h = sha256_hex(g_settings.get("device").value("id", "picocam"));
    return h.substr(0, 8) + "-" + h.substr(8, 4) + "-4" + h.substr(13, 3) + "-8" + h.substr(17, 3) + "-" + h.substr(20, 12);
}

// text content of the first element whose local name is `name`
static std::string tag(const std::string &x, const std::string &name) {
    size_t p = 0;
    while ((p = x.find(name, p)) != std::string::npos) {
        bool start = p > 0 && (x[p - 1] == '<' || x[p - 1] == ':');
        size_t after = p + name.size();
        if (start && after < x.size() && (x[after] == '>' || x[after] == ' ' || x[after] == '/')) {
            size_t lt = x.rfind('<', p);
            if (lt != std::string::npos && x[lt + 1] != '/') {
                size_t gt = x.find('>', p);
                if (gt == std::string::npos) return "";
                if (x[gt - 1] == '/') return "";
                size_t end = x.find('<', gt);
                return end == std::string::npos ? "" : trim(x.substr(gt + 1, end - gt - 1));
            }
        }
        p = after;
    }
    return "";
}

// local name of the first element inside <Body>
static std::string operation(const std::string &x) {
    size_t b = x.find("Body");
    if (b == std::string::npos) return "";
    b = x.find('>', b);
    size_t lt = x.find('<', b);
    while (lt != std::string::npos && (x[lt + 1] == '!' || x[lt + 1] == '?')) lt = x.find('<', lt + 1);
    if (lt == std::string::npos) return "";
    size_t end = x.find_first_of(" />", lt);
    std::string n = x.substr(lt + 1, end - lt - 1);
    size_t c = n.find(':');
    return c == std::string::npos ? n : n.substr(c + 1);
}

static const char *kNs =
    "xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" xmlns:tt=\"http://www.onvif.org/ver10/schema\" "
    "xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\" xmlns:trt=\"http://www.onvif.org/ver10/media/wsdl\" "
    "xmlns:tev=\"http://www.onvif.org/ver10/events/wsdl\" xmlns:timg=\"http://www.onvif.org/ver20/imaging/wsdl\" "
    "xmlns:wsnt=\"http://docs.oasis-open.org/wsn/b-2\" xmlns:wsa=\"http://www.w3.org/2005/08/addressing\" "
    "xmlns:wstop=\"http://docs.oasis-open.org/wsn/t-1\" xmlns:tns1=\"http://www.onvif.org/ver10/topics\" "
    "xmlns:ter=\"http://www.onvif.org/ver10/error\"";

static std::string envelope(const std::string &body, const std::string &header = "") {
    return std::string("<?xml version=\"1.0\" encoding=\"UTF-8\"?><s:Envelope ") + kNs + ">" +
           (header.empty() ? "" : "<s:Header>" + header + "</s:Header>") + "<s:Body>" + body + "</s:Body></s:Envelope>";
}

static void soap(HttpResponse &res, const std::string &body, int code = 200, const std::string &header = "") {
    res.status = code;
    res.headers["Content-Type"] = "application/soap+xml; charset=utf-8";
    res.body = envelope(body, header);
}

static void fault(HttpResponse &res, const std::string &code, const std::string &sub, const std::string &reason, int http = 400) {
    soap(res,
         "<s:Fault><s:Code><s:Value>s:" + code + "</s:Value><s:Subcode><s:Value>" + sub +
             "</s:Value></s:Subcode></s:Code><s:Reason><s:Text xml:lang=\"en\">" + xml_escape(reason) +
             "</s:Text></s:Reason></s:Fault>",
         http);
}

// WS-Security UsernameToken (PasswordDigest or PasswordText), else HTTP auth already resolved by the server
static auth::Role soap_auth(const HttpRequest &req, const std::string &x) {
    if (req.role != auth::NONE) return req.role;
    std::string user = tag(x, "Username");
    if (user.empty()) return auth::NONE;
    std::string pw = tag(x, "Password"), nonce = tag(x, "Nonce"), created = tag(x, "Created");
    std::string secret = auth::secret(user);
    if (secret.empty()) return auth::NONE;
    bool ok;
    if (x.find("PasswordText") != std::string::npos) {
        ok = const_time_eq(pw, secret);
    } else {
        ok = const_time_eq(pw, base64_encode(sha1(base64_decode(nonce) + created + secret)));
    }
    if (!ok) return auth::NONE;
    for (auto &u : auth::list_users())
        if (u.value("name", "") == user) return auth::role_from(u.value("role", ""));
    return auth::NONE;
}

static bool pre_auth(const std::string &op) {
    static const char *ops[] = {"GetSystemDateAndTime", "GetCapabilities", "GetServices", "GetServiceCapabilities",
                                "GetEndpointReference", "GetWsdlUrl", "GetDeviceInformation"};
    for (auto *o : ops)
        if (op == o) return true;
    return false;
}

// ---------------------------------------------------------------- device service

static std::string capabilities_xml() {
    std::string b = base_url();
    return "<tt:Device><tt:XAddr>" + b + "/onvif/device_service</tt:XAddr><tt:Network><tt:IPFilter>false</tt:IPFilter>"
           "<tt:ZeroConfiguration>false</tt:ZeroConfiguration><tt:IPVersion6>false</tt:IPVersion6><tt:DynDNS>false</tt:DynDNS>"
           "</tt:Network><tt:System><tt:DiscoveryResolve>false</tt:DiscoveryResolve><tt:DiscoveryBye>true</tt:DiscoveryBye>"
           "<tt:RemoteDiscovery>false</tt:RemoteDiscovery><tt:SystemBackup>false</tt:SystemBackup><tt:SystemLogging>false</tt:SystemLogging>"
           "<tt:FirmwareUpgrade>false</tt:FirmwareUpgrade><tt:SupportedVersions><tt:Major>2</tt:Major><tt:Minor>60</tt:Minor>"
           "</tt:SupportedVersions></tt:System><tt:Security><tt:TLS1.1>false</tt:TLS1.1><tt:TLS1.2>false</tt:TLS1.2>"
           "<tt:OnboardKeyGeneration>false</tt:OnboardKeyGeneration><tt:AccessPolicyConfig>false</tt:AccessPolicyConfig>"
           "<tt:X.509Token>false</tt:X.509Token><tt:SAMLToken>false</tt:SAMLToken><tt:KerberosToken>false</tt:KerberosToken>"
           "<tt:RELToken>false</tt:RELToken></tt:Security></tt:Device>"
           "<tt:Events><tt:XAddr>" + b + "/onvif/events_service</tt:XAddr><tt:WSSubscriptionPolicySupport>false</tt:WSSubscriptionPolicySupport>"
           "<tt:WSPullPointSupport>true</tt:WSPullPointSupport><tt:WSPausableSubscriptionManagerInterfaceSupport>false"
           "</tt:WSPausableSubscriptionManagerInterfaceSupport></tt:Events>"
           "<tt:Imaging><tt:XAddr>" + b + "/onvif/imaging_service</tt:XAddr></tt:Imaging>"
           "<tt:Media><tt:XAddr>" + b + "/onvif/media_service</tt:XAddr><tt:StreamingCapabilities><tt:RTPMulticast>false</tt:RTPMulticast>"
           "<tt:RTP_TCP>true</tt:RTP_TCP><tt:RTP_RTSP_TCP>true</tt:RTP_RTSP_TCP></tt:StreamingCapabilities></tt:Media>";
}

static void device_service(HttpRequest &req, HttpResponse &res, const std::string &op, const std::string &x) {
    json dev = sys::device_info();
    if (op == "GetSystemDateAndTime") {
        time_t t = time(nullptr);
        struct tm u, l;
        gmtime_r(&t, &u);
        localtime_r(&t, &l);
        auto dt = [](const struct tm &m) {
            return "<tt:Time><tt:Hour>" + std::to_string(m.tm_hour) + "</tt:Hour><tt:Minute>" + std::to_string(m.tm_min) +
                   "</tt:Minute><tt:Second>" + std::to_string(m.tm_sec) + "</tt:Second></tt:Time><tt:Date><tt:Year>" +
                   std::to_string(m.tm_year + 1900) + "</tt:Year><tt:Month>" + std::to_string(m.tm_mon + 1) + "</tt:Month><tt:Day>" +
                   std::to_string(m.tm_mday) + "</tt:Day></tt:Date>";
        };
        std::string tz = jget<std::string>(g_settings.get("time"), "timezone", "UTC0");
        return soap(res, "<tds:GetSystemDateAndTimeResponse><tds:SystemDateAndTime><tt:DateTimeType>" +
                             std::string(jget<bool>(g_settings.get("time"), "ntp", true) ? "NTP" : "Manual") +
                             "</tt:DateTimeType><tt:DaylightSavings>false</tt:DaylightSavings><tt:TimeZone><tt:TZ>" + xml_escape(tz) +
                             "</tt:TZ></tt:TimeZone><tt:UTCDateTime>" + dt(u) + "</tt:UTCDateTime><tt:LocalDateTime>" + dt(l) +
                             "</tt:LocalDateTime></tds:SystemDateAndTime></tds:GetSystemDateAndTimeResponse>");
    }
    if (op == "GetCapabilities")
        return soap(res, "<tds:GetCapabilitiesResponse><tds:Capabilities>" + capabilities_xml() + "</tds:Capabilities></tds:GetCapabilitiesResponse>");
    if (op == "GetServices") {
        std::string b = base_url();
        auto svc = [&](const char *ns, const char *path) {
            return std::string("<tds:Service><tds:Namespace>") + ns + "</tds:Namespace><tds:XAddr>" + b + path +
                   "</tds:XAddr><tds:Version><tt:Major>2</tt:Major><tt:Minor>60</tt:Minor></tds:Version></tds:Service>";
        };
        return soap(res, "<tds:GetServicesResponse>" + svc("http://www.onvif.org/ver10/device/wsdl", "/onvif/device_service") +
                             svc("http://www.onvif.org/ver10/media/wsdl", "/onvif/media_service") +
                             svc("http://www.onvif.org/ver10/events/wsdl", "/onvif/events_service") +
                             svc("http://www.onvif.org/ver20/imaging/wsdl", "/onvif/imaging_service") + "</tds:GetServicesResponse>");
    }
    if (op == "GetServiceCapabilities")
        return soap(res, "<tds:GetServiceCapabilitiesResponse><tds:Capabilities><tds:Network IPFilter=\"false\" ZeroConfiguration=\"false\" "
                         "IPVersion6=\"false\" DynDNS=\"false\" NTP=\"1\"/><tds:Security UsernameToken=\"true\" HttpDigest=\"true\"/>"
                         "<tds:System DiscoveryBye=\"true\" SystemLogging=\"false\" FirmwareUpgrade=\"false\"/></tds:Capabilities>"
                         "</tds:GetServiceCapabilitiesResponse>");
    if (op == "GetDeviceInformation")
        return soap(res, "<tds:GetDeviceInformationResponse><tds:Manufacturer>PicoCam</tds:Manufacturer><tds:Model>" +
                             xml_escape(dev.value("model", "")) + "</tds:Model><tds:FirmwareVersion>" + xml_escape(dev.value("firmware", "")) +
                             "</tds:FirmwareVersion><tds:SerialNumber>" + xml_escape(dev.value("id", "")) +
                             "</tds:SerialNumber><tds:HardwareId>RV1103-SC3336</tds:HardwareId></tds:GetDeviceInformationResponse>");
    if (op == "GetScopes") {
        std::string name = dev.value("name", "PicoCam");
        for (auto &c : name)
            if (c == ' ') c = '_';
        auto sc = [](const std::string &s) { return "<tds:Scopes><tt:ScopeDef>Fixed</tt:ScopeDef><tt:ScopeItem>" + xml_escape(s) + "</tt:ScopeItem></tds:Scopes>"; };
        return soap(res, "<tds:GetScopesResponse>" + sc("onvif://www.onvif.org/type/video_encoder") + sc("onvif://www.onvif.org/Profile/Streaming") +
                             sc("onvif://www.onvif.org/name/" + name) + sc("onvif://www.onvif.org/hardware/PicoCam") +
                             sc("onvif://www.onvif.org/location/" + dev.value("location", "")) + "</tds:GetScopesResponse>");
    }
    if (op == "GetHostname") {
        char h[64] = {0};
        gethostname(h, 63);
        return soap(res, std::string("<tds:GetHostnameResponse><tds:HostnameInformation><tt:FromDHCP>false</tt:FromDHCP><tt:Name>") + h +
                             "</tt:Name></tds:HostnameInformation></tds:GetHostnameResponse>");
    }
    if (op == "GetNetworkInterfaces") {
        std::string ifn = primary_iface();
        int prefix = 24;
        for (auto &i : sys::network_status().value("interfaces", json::array()))
            if (i.value("name", "") == ifn) {
                in_addr m;
                if (inet_pton(AF_INET, i.value("netmask", "").c_str(), &m) == 1) prefix = __builtin_popcount(ntohl(m.s_addr));
            }
        return soap(res, "<tds:GetNetworkInterfacesResponse><tds:NetworkInterfaces token=\"" + ifn +
                             "\"><tt:Enabled>true</tt:Enabled><tt:Info><tt:Name>" + ifn + "</tt:Name><tt:HwAddress>" + iface_mac(ifn) +
                             "</tt:HwAddress><tt:MTU>1500</tt:MTU></tt:Info><tt:IPv4><tt:Enabled>true</tt:Enabled><tt:Config><tt:Manual>"
                             "<tt:Address>" + ip() + "</tt:Address><tt:PrefixLength>" + std::to_string(prefix) + "</tt:PrefixLength></tt:Manual><tt:DHCP>" +
                             (jget<std::string>(g_settings.get("network"), "mode", "dhcp") == "dhcp" ? "true" : "false") +
                             "</tt:DHCP></tt:Config></tt:IPv4></tds:NetworkInterfaces></tds:GetNetworkInterfacesResponse>");
    }
    if (op == "GetNetworkProtocols")
        return soap(res, "<tds:GetNetworkProtocolsResponse><tds:NetworkProtocols><tt:Name>HTTP</tt:Name><tt:Enabled>true</tt:Enabled><tt:Port>" +
                             std::to_string(http_port()) + "</tt:Port></tds:NetworkProtocols><tds:NetworkProtocols><tt:Name>RTSP</tt:Name>"
                             "<tt:Enabled>true</tt:Enabled><tt:Port>" + std::to_string(rtsp_port()) +
                             "</tt:Port></tds:NetworkProtocols></tds:GetNetworkProtocolsResponse>");
    if (op == "GetDNS") return soap(res, "<tds:GetDNSResponse><tds:DNSInformation><tt:FromDHCP>true</tt:FromDHCP></tds:DNSInformation></tds:GetDNSResponse>");
    if (op == "GetNTP")
        return soap(res, "<tds:GetNTPResponse><tds:NTPInformation><tt:FromDHCP>false</tt:FromDHCP><tt:NTPManual><tt:Type>DNS</tt:Type><tt:DNSname>" +
                             xml_escape(jget<std::string>(g_settings.get("time"), "ntp_server", "")) +
                             "</tt:DNSname></tt:NTPManual></tds:NTPInformation></tds:GetNTPResponse>");
    if (op == "GetNetworkDefaultGateway")
        return soap(res, "<tds:GetNetworkDefaultGatewayResponse><tds:NetworkGateway><tt:IPv4Address>" +
                             sys::network_status().value("gateway", "") + "</tt:IPv4Address></tds:NetworkGateway></tds:GetNetworkDefaultGatewayResponse>");
    if (op == "GetDiscoveryMode")
        return soap(res, "<tds:GetDiscoveryModeResponse><tds:DiscoveryMode>Discoverable</tds:DiscoveryMode></tds:GetDiscoveryModeResponse>");
    if (op == "GetUsers") {
        std::string u;
        for (auto &x2 : auth::list_users()) {
            std::string r = x2.value("role", "viewer");
            u += "<tds:User><tt:Username>" + xml_escape(x2.value("name", "")) + "</tt:Username><tt:UserLevel>" +
                 (r == "admin" ? "Administrator" : r == "operator" ? "Operator" : "User") + "</tt:UserLevel></tds:User>";
        }
        return soap(res, "<tds:GetUsersResponse>" + u + "</tds:GetUsersResponse>");
    }
    if (op == "SystemReboot") {
        if (req.role < auth::ADMIN) return fault(res, "Sender", "ter:NotAuthorized", "admin required");
        sys::reboot(2000);
        return soap(res, "<tds:SystemRebootResponse><tds:Message>Rebooting</tds:Message></tds:SystemRebootResponse>");
    }
    if (op == "SetSystemDateAndTime") {
        // NVRs push their clock: accept manual UTC time
        std::string utc = x.substr(std::min(x.size(), x.find("UTCDateTime") == std::string::npos ? x.size() : x.find("UTCDateTime")));
        if (!utc.empty() && req.role >= auth::ADMIN) {
            struct tm m{};
            m.tm_year = atoi(tag(utc, "Year").c_str()) - 1900;
            m.tm_mon = atoi(tag(utc, "Month").c_str()) - 1;
            m.tm_mday = atoi(tag(utc, "Day").c_str());
            m.tm_hour = atoi(tag(utc, "Hour").c_str());
            m.tm_min = atoi(tag(utc, "Minute").c_str());
            m.tm_sec = atoi(tag(utc, "Second").c_str());
            if (m.tm_year > 100) sys::set_clock((int64_t)timegm(&m) * 1000);
        }
        return soap(res, "<tds:SetSystemDateAndTimeResponse/>");
    }
    fault(res, "Receiver", "ter:ActionNotSupported", "operation " + op + " not supported");
}

// ---------------------------------------------------------------- media service

static std::string encoder_xml(int idx) {
    Stream *s = stream_by_index(idx);
    json v = g_settings.get("video")[idx ? "sub" : "main"];
    std::string enc = s->codec() == Codec::H265 ? "H265" : "H264";
    std::string tok = idx ? "venc_sub" : "venc_main";
    return "<tt:Name>" + tok + "</tt:Name><tt:UseCount>1</tt:UseCount><tt:Encoding>" + enc +
           "</tt:Encoding><tt:Resolution><tt:Width>" + std::to_string(s->width()) + "</tt:Width><tt:Height>" + std::to_string(s->height()) +
           "</tt:Height></tt:Resolution><tt:Quality>4</tt:Quality><tt:RateControl><tt:FrameRateLimit>" + std::to_string(s->fps()) +
           "</tt:FrameRateLimit><tt:EncodingInterval>1</tt:EncodingInterval><tt:BitrateLimit>" + std::to_string(jget<int>(v, "bitrate", 2048)) +
           "</tt:BitrateLimit></tt:RateControl>" +
           (s->codec() == Codec::H264 ? "<tt:H264><tt:GovLength>" + std::to_string(jget<int>(v, "gop", 50)) +
                                            "</tt:GovLength><tt:H264Profile>" + (idx ? "Main" : "High") + "</tt:H264Profile></tt:H264>"
                                      : "<tt:H265><tt:GovLength>" + std::to_string(jget<int>(v, "gop", 50)) +
                                            "</tt:GovLength><tt:H265Profile>Main</tt:H265Profile></tt:H265>") +
           "<tt:Multicast><tt:Address><tt:Type>IPv4</tt:Type><tt:IPv4Address>0.0.0.0</tt:IPv4Address></tt:Address><tt:Port>0</tt:Port>"
           "<tt:TTL>0</tt:TTL><tt:AutoStart>false</tt:AutoStart></tt:Multicast><tt:SessionTimeout>PT60S</tt:SessionTimeout>";
}

static std::string vsc_xml() {
    return "<tt:Name>vsc0</tt:Name><tt:UseCount>2</tt:UseCount><tt:SourceToken>vs0</tt:SourceToken><tt:Bounds x=\"0\" y=\"0\" width=\"" +
           std::to_string(g_main_stream.width()) + "\" height=\"" + std::to_string(g_main_stream.height()) + "\"/>";
}

static std::string profile_xml(int idx) {
    std::string tok = idx ? "profile_sub" : "profile_main";
    return "<trt:Profiles token=\"" + tok + "\" fixed=\"true\"><tt:Name>" + (idx ? "SubStream" : "MainStream") +
           "</tt:Name><tt:VideoSourceConfiguration token=\"vsc0\">" + vsc_xml() + "</tt:VideoSourceConfiguration>" +
           "<tt:VideoEncoderConfiguration token=\"" + (idx ? "venc_sub" : "venc_main") + "\">" + encoder_xml(idx) +
           "</tt:VideoEncoderConfiguration></trt:Profiles>";
}

static int profile_index(const std::string &x) {
    std::string t = tag(x, "ProfileToken");
    if (t.empty()) t = tag(x, "ConfigurationToken");
    return t.find("sub") != std::string::npos ? 1 : 0;
}

static void media_service(HttpRequest &, HttpResponse &res, const std::string &op, const std::string &x) {
    if (op == "GetProfiles") return soap(res, "<trt:GetProfilesResponse>" + profile_xml(0) + profile_xml(1) + "</trt:GetProfilesResponse>");
    if (op == "GetProfile") {
        std::string p = profile_xml(profile_index(x));
        p.replace(0, 13, "<trt:Profile");
        p.replace(p.size() - 15, 15, "</trt:Profile>");
        return soap(res, "<trt:GetProfileResponse>" + p + "</trt:GetProfileResponse>");
    }
    if (op == "GetStreamUri") {
        std::string uri = "rtsp://" + ip() + (rtsp_port() == 554 ? "" : ":" + std::to_string(rtsp_port())) +
                          (profile_index(x) ? "/live/sub" : "/live/main");
        return soap(res, "<trt:GetStreamUriResponse><trt:MediaUri><tt:Uri>" + uri +
                             "</tt:Uri><tt:InvalidAfterConnect>false</tt:InvalidAfterConnect><tt:InvalidAfterReboot>false"
                             "</tt:InvalidAfterReboot><tt:Timeout>PT0S</tt:Timeout></trt:MediaUri></trt:GetStreamUriResponse>");
    }
    if (op == "GetSnapshotUri")
        return soap(res, "<trt:GetSnapshotUriResponse><trt:MediaUri><tt:Uri>" + base_url() +
                             "/snapshot.jpg</tt:Uri><tt:InvalidAfterConnect>false</tt:InvalidAfterConnect><tt:InvalidAfterReboot>false"
                             "</tt:InvalidAfterReboot><tt:Timeout>PT0S</tt:Timeout></trt:MediaUri></trt:GetSnapshotUriResponse>");
    if (op == "GetVideoSources")
        return soap(res, "<trt:GetVideoSourcesResponse><trt:VideoSources token=\"vs0\"><tt:Framerate>" + std::to_string(g_main_stream.fps()) +
                             "</tt:Framerate><tt:Resolution><tt:Width>" + std::to_string(g_main_stream.width()) + "</tt:Width><tt:Height>" +
                             std::to_string(g_main_stream.height()) + "</tt:Height></tt:Resolution></trt:VideoSources></trt:GetVideoSourcesResponse>");
    if (op == "GetVideoSourceConfigurations")
        return soap(res, "<trt:GetVideoSourceConfigurationsResponse><trt:Configurations token=\"vsc0\">" + vsc_xml() +
                             "</trt:Configurations></trt:GetVideoSourceConfigurationsResponse>");
    if (op == "GetVideoSourceConfiguration")
        return soap(res, "<trt:GetVideoSourceConfigurationResponse><trt:Configuration token=\"vsc0\">" + vsc_xml() +
                             "</trt:Configuration></trt:GetVideoSourceConfigurationResponse>");
    if (op == "GetVideoEncoderConfigurations" || op == "GetCompatibleVideoEncoderConfigurations")
        return soap(res, "<trt:" + op + "Response><trt:Configurations token=\"venc_main\">" + encoder_xml(0) +
                             "</trt:Configurations><trt:Configurations token=\"venc_sub\">" + encoder_xml(1) + "</trt:Configurations></trt:" + op +
                             "Response>");
    if (op == "GetVideoEncoderConfiguration") {
        int i = profile_index(x);
        return soap(res, std::string("<trt:GetVideoEncoderConfigurationResponse><trt:Configuration token=\"") + (i ? "venc_sub" : "venc_main") +
                             "\">" + encoder_xml(i) + "</trt:Configuration></trt:GetVideoEncoderConfigurationResponse>");
    }
    if (op == "GetVideoEncoderConfigurationOptions") {
        int i = profile_index(x);
        Stream *s = stream_by_index(i);
        std::string codec_tag = s->codec() == Codec::H265 ? "H265" : "H264";
        std::string profiles = s->codec() == Codec::H265
                                   ? "<tt:H265ProfilesSupported>Main</tt:H265ProfilesSupported>"
                                   : "<tt:H264ProfilesSupported>Main</tt:H264ProfilesSupported><tt:H264ProfilesSupported>High</tt:H264ProfilesSupported>";
        return soap(res, "<trt:GetVideoEncoderConfigurationOptionsResponse><trt:Options><tt:QualityRange><tt:Min>1</tt:Min><tt:Max>5</tt:Max>"
                         "</tt:QualityRange><tt:" + codec_tag + "><tt:ResolutionsAvailable><tt:Width>" + std::to_string(s->width()) + "</tt:Width><tt:Height>" +
                             std::to_string(s->height()) + "</tt:Height></tt:ResolutionsAvailable><tt:GovLengthRange><tt:Min>1</tt:Min><tt:Max>300"
                             "</tt:Max></tt:GovLengthRange><tt:FrameRateRange><tt:Min>1</tt:Min><tt:Max>30</tt:Max></tt:FrameRateRange>"
                             "<tt:EncodingIntervalRange><tt:Min>1</tt:Min><tt:Max>1</tt:Max></tt:EncodingIntervalRange>" + profiles + "</tt:" + codec_tag +
                             "></trt:Options></trt:GetVideoEncoderConfigurationOptionsResponse>");
    }
    if (op == "SetVideoEncoderConfiguration") return soap(res, "<trt:SetVideoEncoderConfigurationResponse/>");
    if (op == "GetAudioSources" || op == "GetAudioEncoderConfigurations" || op == "GetAudioOutputs" ||
        op == "GetVideoAnalyticsConfigurations" || op == "GetMetadataConfigurations" || op == "GetOSDs")
        return soap(res, "<trt:" + op + "Response/>");
    if (op == "GetServiceCapabilities")
        return soap(res, "<trt:GetServiceCapabilitiesResponse><trt:Capabilities SnapshotUri=\"true\" Rotation=\"false\" OSD=\"false\">"
                         "<trt:ProfileCapabilities MaximumNumberOfProfiles=\"2\"/><trt:StreamingCapabilities RTPMulticast=\"false\" "
                         "RTP_TCP=\"true\" RTP_RTSP_TCP=\"true\"/></trt:Capabilities></trt:GetServiceCapabilitiesResponse>");
    fault(res, "Receiver", "ter:ActionNotSupported", "operation " + op + " not supported");
}

// ---------------------------------------------------------------- imaging

static void imaging_service(HttpRequest &, HttpResponse &res, const std::string &op, const std::string &) {
    json img = g_settings.get("image");
    if (op == "GetImagingSettings")
        return soap(res, "<timg:GetImagingSettingsResponse><timg:ImagingSettings><tt:Brightness>" + std::to_string(jget<int>(img, "brightness", 50)) +
                             "</tt:Brightness><tt:ColorSaturation>" + std::to_string(jget<int>(img, "saturation", 50)) +
                             "</tt:ColorSaturation><tt:Contrast>" + std::to_string(jget<int>(img, "contrast", 50)) +
                             "</tt:Contrast><tt:Sharpness>" + std::to_string(jget<int>(img, "sharpness", 50)) +
                             "</tt:Sharpness></timg:ImagingSettings></timg:GetImagingSettingsResponse>");
    if (op == "GetOptions")
        return soap(res, "<timg:GetOptionsResponse><timg:ImagingOptions><tt:Brightness><tt:Min>0</tt:Min><tt:Max>100</tt:Max></tt:Brightness>"
                         "<tt:ColorSaturation><tt:Min>0</tt:Min><tt:Max>100</tt:Max></tt:ColorSaturation><tt:Contrast><tt:Min>0</tt:Min>"
                         "<tt:Max>100</tt:Max></tt:Contrast><tt:Sharpness><tt:Min>0</tt:Min><tt:Max>100</tt:Max></tt:Sharpness>"
                         "</timg:ImagingOptions></timg:GetOptionsResponse>");
    if (op == "GetServiceCapabilities")
        return soap(res, "<timg:GetServiceCapabilitiesResponse><timg:Capabilities ImageStabilization=\"false\"/></timg:GetServiceCapabilitiesResponse>");
    fault(res, "Receiver", "ter:ActionNotSupported", "operation " + op + " not supported");
}

// ---------------------------------------------------------------- events (PullPoint)

struct Sub {
    std::deque<std::string> msgs;
    int64_t expires = 0;
    std::vector<std::pair<int64_t, std::string>> delayed;  // (due ms, message): motion "false" after an alarm
};
static std::mutex g_emu;
static std::map<std::string, Sub> g_subs;

static std::string notify_xml(const std::string &topic, const std::string &src, const std::string &data, const std::string &op = "Changed") {
    return "<wsnt:NotificationMessage><wsnt:Topic Dialect=\"http://www.onvif.org/ver10/tev/topicExpression/ConcreteSet\">" + topic +
           "</wsnt:Topic><wsnt:Message><tt:Message UtcTime=\"" + iso8601(wall_ms()) + "\" PropertyOperation=\"" + op +
           "\"><tt:Source>" + src + "</tt:Source><tt:Data>" + data + "</tt:Data></tt:Message></wsnt:Message></wsnt:NotificationMessage>";
}

static void on_event(const json &rec) {
    std::string src = "<tt:SimpleItem Name=\"VideoSourceConfigurationToken\" Value=\"vsc0\"/><tt:SimpleItem Name=\"VideoAnalyticsConfigurationToken\" "
                      "Value=\"va0\"/><tt:SimpleItem Name=\"Rule\" Value=\"" + xml_escape(rec.value("rule", "Motion")) + "\"/>";
    std::string on = notify_xml("tns1:RuleEngine/CellMotionDetector/Motion", src, "<tt:SimpleItem Name=\"IsMotion\" Value=\"true\"/>");
    std::string off = notify_xml("tns1:RuleEngine/CellMotionDetector/Motion", src, "<tt:SimpleItem Name=\"IsMotion\" Value=\"false\"/>");
    std::string vs = "<tt:SimpleItem Name=\"Source\" Value=\"vs0\"/>";
    std::string alarm_on = notify_xml("tns1:VideoSource/MotionAlarm", vs, "<tt:SimpleItem Name=\"State\" Value=\"true\"/>");
    std::string alarm_off = notify_xml("tns1:VideoSource/MotionAlarm", vs, "<tt:SimpleItem Name=\"State\" Value=\"false\"/>");
    std::lock_guard<std::mutex> lk(g_emu);
    int64_t due = now_ms() + 5000;
    for (auto &kv : g_subs) {
        Sub &s = kv.second;
        if (s.msgs.size() > 60) s.msgs.erase(s.msgs.begin(), s.msgs.begin() + 20);
        s.msgs.push_back(on);
        s.msgs.push_back(alarm_on);
        s.delayed.push_back({due, off});
        s.delayed.push_back({due, alarm_off});
    }
}

static void events_service(HttpRequest &req, HttpResponse &res, const std::string &op, const std::string &x) {
    std::string b = base_url();
    if (op == "GetServiceCapabilities")
        return soap(res, "<tev:GetServiceCapabilitiesResponse><tev:Capabilities WSSubscriptionPolicySupport=\"false\" "
                         "WSPullPointSupport=\"true\" WSPausableSubscriptionManagerInterfaceSupport=\"false\" MaxNotificationProducers=\"0\" "
                         "MaxPullPoints=\"4\"/></tev:GetServiceCapabilitiesResponse>");
    if (op == "GetEventProperties")
        return soap(res, "<tev:GetEventPropertiesResponse><tev:TopicNamespaceLocation>http://www.onvif.org/onvif/ver10/topics/topicns.xml"
                         "</tev:TopicNamespaceLocation><wsnt:FixedTopicSet>true</wsnt:FixedTopicSet><wstop:TopicSet><tns1:RuleEngine>"
                         "<CellMotionDetector><Motion wstop:topic=\"true\"><tt:MessageDescription IsProperty=\"true\"><tt:Source>"
                         "<tt:SimpleItemDescription Name=\"VideoSourceConfigurationToken\" Type=\"tt:ReferenceToken\"/><tt:SimpleItemDescription "
                         "Name=\"VideoAnalyticsConfigurationToken\" Type=\"tt:ReferenceToken\"/><tt:SimpleItemDescription Name=\"Rule\" "
                         "Type=\"xs:string\"/></tt:Source><tt:Data><tt:SimpleItemDescription Name=\"IsMotion\" Type=\"xs:boolean\"/></tt:Data>"
                         "</tt:MessageDescription></Motion></CellMotionDetector></tns1:RuleEngine><tns1:VideoSource><MotionAlarm "
                         "wstop:topic=\"true\"><tt:MessageDescription IsProperty=\"true\"><tt:Source><tt:SimpleItemDescription Name=\"Source\" "
                         "Type=\"tt:ReferenceToken\"/></tt:Source><tt:Data><tt:SimpleItemDescription Name=\"State\" Type=\"xs:boolean\"/>"
                         "</tt:Data></tt:MessageDescription></MotionAlarm></tns1:VideoSource></wstop:TopicSet><wsnt:TopicExpressionDialect>"
                         "http://www.onvif.org/ver10/tev/topicExpression/ConcreteSet</wsnt:TopicExpressionDialect><tev:MessageContentFilterDialect>"
                         "http://www.onvif.org/ver10/tev/messageContentFilter/ItemFilter</tev:MessageContentFilterDialect>"
                         "<tev:MessageContentSchemaLocation>http://www.onvif.org/onvif/ver10/schema/onvif.xsd</tev:MessageContentSchemaLocation>"
                         "</tev:GetEventPropertiesResponse>");
    if (op == "CreatePullPointSubscription") {
        std::string id = random_token(8);
        int64_t now = wall_ms();
        {
            std::lock_guard<std::mutex> lk(g_emu);
            // expire stale subscriptions; cap the number held
            for (auto it = g_subs.begin(); it != g_subs.end();) it = it->second.expires < now ? g_subs.erase(it) : ++it;
            if (g_subs.size() >= 8) g_subs.erase(g_subs.begin());
            g_subs[id].expires = now + 120000;
        }
        return soap(res, "<tev:CreatePullPointSubscriptionResponse><tev:SubscriptionReference><wsa:Address>" + b + "/onvif/events_service/pull/" + id +
                             "</wsa:Address></tev:SubscriptionReference><wsnt:CurrentTime>" + iso8601(now) + "</wsnt:CurrentTime>"
                             "<wsnt:TerminationTime>" + iso8601(now + 120000) + "</wsnt:TerminationTime></tev:CreatePullPointSubscriptionResponse>");
    }
    // operations on a subscription: /onvif/events_service/pull/<id>
    std::string id = req.path_args.size() >= 2 && req.path_args[0] == "pull" ? req.path_args[1] : "";
    if (op == "PullMessages") {
        int timeout_s = 10;
        std::string to = tag(x, "Timeout");  // PT5S / PT1M
        if (starts_with(to, "PT")) {
            int v = atoi(to.c_str() + 2);
            timeout_s = to.find('M') != std::string::npos ? v * 60 : v;
        }
        timeout_s = std::max(1, std::min(20, timeout_s));
        int limit = std::max(1, std::min(50, atoi(tag(x, "MessageLimit").c_str())));
        std::string msgs;
        int64_t end = now_ms() + timeout_s * 1000;
        for (;;) {
            {
                std::lock_guard<std::mutex> lk(g_emu);
                auto it = g_subs.find(id);
                if (it == g_subs.end()) return fault(res, "Sender", "ter:InvalidArgVal", "unknown subscription");
                Sub &s = it->second;
                s.expires = std::max(s.expires, wall_ms() + 60000);
                int64_t t = now_ms();
                for (auto d = s.delayed.begin(); d != s.delayed.end();) {
                    if (d->first <= t) {
                        s.msgs.push_back(d->second);
                        d = s.delayed.erase(d);
                    } else {
                        ++d;
                    }
                }
                int n = 0;
                while (!s.msgs.empty() && n < limit) {
                    msgs += s.msgs.front();
                    s.msgs.pop_front();
                    ++n;
                }
            }
            if (!msgs.empty() || now_ms() >= end) break;
            usleep(200 * 1000);
        }
        int64_t now = wall_ms();
        return soap(res, "<tev:PullMessagesResponse><tev:CurrentTime>" + iso8601(now) + "</tev:CurrentTime><tev:TerminationTime>" +
                             iso8601(now + 60000) + "</tev:TerminationTime>" + msgs + "</tev:PullMessagesResponse>");
    }
    if (op == "Renew") {
        std::lock_guard<std::mutex> lk(g_emu);
        if (g_subs.count(id)) g_subs[id].expires = wall_ms() + 120000;
        int64_t now = wall_ms();
        return soap(res, "<wsnt:RenewResponse><wsnt:TerminationTime>" + iso8601(now + 120000) + "</wsnt:TerminationTime><wsnt:CurrentTime>" +
                             iso8601(now) + "</wsnt:CurrentTime></wsnt:RenewResponse>");
    }
    if (op == "Unsubscribe") {
        std::lock_guard<std::mutex> lk(g_emu);
        g_subs.erase(id);
        return soap(res, "<wsnt:UnsubscribeResponse/>");
    }
    if (op == "SetSynchronizationPoint") return soap(res, "<tev:SetSynchronizationPointResponse/>");
    fault(res, "Receiver", "ter:ActionNotSupported", "operation " + op + " not supported");
}

// ---------------------------------------------------------------- dispatch

static void handle(HttpRequest &req, HttpResponse &res, int svc) {
    if (!jget<bool>(g_settings.get("onvif"), "enabled", true)) return res.error(404, "ONVIF disabled");
    std::string x = req.body;
    std::string op = operation(x);
    if (op.empty()) return fault(res, "Sender", "ter:InvalidArgVal", "no operation");
    if (!pre_auth(op)) {
        if (req.role == auth::NONE && auth::locked_out(req.peer))
            return fault(res, "Sender", "ter:NotAuthorized", "too many failed attempts, try again later", 429);
        auth::Role r = soap_auth(req, x);
        if (r != auth::NONE && req.role == auth::NONE) auth::note_success(req.peer);
        if (r == auth::NONE) {
            if (x.find("UsernameToken") != std::string::npos) auth::note_failure(req.peer);  // credentials were given and wrong
            fault(res, "Sender", "ter:NotAuthorized", "authentication required", x.find("UsernameToken") != std::string::npos ? 400 : 401);
            if (x.find("UsernameToken") == std::string::npos) res.headers["WWW-Authenticate"] = digest_challenge();
            return;
        }
        req.role = r;
    }
    switch (svc) {
        case 0: return device_service(req, res, op, x);
        case 1: return media_service(req, res, op, x);
        case 2: return events_service(req, res, op, x);
        default: return imaging_service(req, res, op, x);
    }
}

// ---------------------------------------------------------------- WS-Discovery

static std::atomic<bool> g_run{false};
static std::thread g_th;
static int g_udp = -1;

static std::string probe_match(const std::string &relates) {
    std::string types = "dn:NetworkVideoTransmitter tds:Device";
    std::string name = g_settings.get("device").value("name", "PicoCam");
    for (auto &c : name)
        if (c == ' ') c = '_';
    std::string scopes = "onvif://www.onvif.org/type/video_encoder onvif://www.onvif.org/Profile/Streaming onvif://www.onvif.org/name/" + name +
                         " onvif://www.onvif.org/hardware/PicoCam";
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?><s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
           "xmlns:a=\"http://schemas.xmlsoap.org/ws/2004/08/addressing\" xmlns:d=\"http://schemas.xmlsoap.org/ws/2005/04/discovery\" "
           "xmlns:dn=\"http://www.onvif.org/ver10/network/wsdl\" xmlns:tds=\"http://www.onvif.org/ver10/device/wsdl\"><s:Header>"
           "<a:MessageID>uuid:" + random_token(4) + "-" + random_token(2) + "-" + random_token(2) + "-" + random_token(2) + "-" + random_token(6) +
           "</a:MessageID><a:RelatesTo>" + xml_escape(relates) + "</a:RelatesTo><a:To>http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous</a:To>"
           "<a:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/ProbeMatches</a:Action></s:Header><s:Body><d:ProbeMatches><d:ProbeMatch>"
           "<a:EndpointReference><a:Address>urn:uuid:" + device_uuid() + "</a:Address></a:EndpointReference><d:Types>" + types +
           "</d:Types><d:Scopes>" + xml_escape(scopes) + "</d:Scopes><d:XAddrs>" + base_url() + "/onvif/device_service</d:XAddrs>"
           "<d:MetadataVersion>1</d:MetadataVersion></d:ProbeMatch></d:ProbeMatches></s:Body></s:Envelope>";
}

static void discovery_loop() {
    prctl(PR_SET_NAME, "ws-discovery", 0, 0, 0);
    char buf[8192];
    std::string joined_ip;
    while (g_run) {
        // (re)join the multicast group on the active interface (the address can change)
        std::string cur = ip();
        if (!cur.empty() && cur != joined_ip) {
            ip_mreq m{};
            inet_pton(AF_INET, "239.255.255.250", &m.imr_multiaddr);
            inet_pton(AF_INET, cur.c_str(), &m.imr_interface);
            setsockopt(g_udp, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, sizeof(m));
            joined_ip = cur;
        }
        sockaddr_in from{};
        socklen_t fl = sizeof(from);
        ssize_t n = recvfrom(g_udp, buf, sizeof(buf) - 1, 0, (sockaddr *)&from, &fl);
        if (n <= 0) continue;
        buf[n] = 0;
        std::string x(buf, n);
        if (x.find("Probe") == std::string::npos || x.find("ProbeMatch") != std::string::npos) continue;
        if (!jget<bool>(g_settings.get("onvif"), "discovery", true) || !jget<bool>(g_settings.get("onvif"), "enabled", true)) continue;
        // answer probes for devices/transmitters, or untyped probes
        std::string types = tag(x, "Types");
        if (!types.empty() && types.find("NetworkVideoTransmitter") == std::string::npos && types.find("Device") == std::string::npos) continue;
        std::string reply = probe_match(tag(x, "MessageID"));
        sendto(g_udp, reply.data(), reply.size(), 0, (sockaddr *)&from, fl);
    }
}

void start() {
    for (int i = 0; i < 4; ++i) {
        static const char *paths[] = {"/onvif/device_service", "/onvif/media_service", "/onvif/events_service", "/onvif/imaging_service"};
        int svc = i;
        g_http.route("POST", paths[i], auth::NONE, [svc](HttpRequest &q, HttpResponse &r) { handle(q, r, svc); }, false, svc == 2);
    }
    // PullMessages long-polls: async so it does not block the web server
    g_http.route("POST", "/onvif/events_service/", auth::NONE, [](HttpRequest &q, HttpResponse &r) { handle(q, r, 2); }, false, true);
    events::add_listener(on_event);

    g_udp = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    int one = 1;
    setsockopt(g_udp, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    timeval to{1, 0};
    setsockopt(g_udp, SOL_SOCKET, SO_RCVTIMEO, &to, sizeof(to));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(3702);
    a.sin_addr.s_addr = INADDR_ANY;
    if (bind(g_udp, (sockaddr *)&a, sizeof(a)) != 0) {
        LOGW(TAG, "WS-Discovery port 3702 unavailable");
        close(g_udp);
        g_udp = -1;
        return;
    }
    g_run = true;
    g_th = std::thread(discovery_loop);
    LOGI(TAG, "ONVIF ready at %s/onvif/device_service (uuid %s)", base_url().c_str(), device_uuid().c_str());
}

void stop() {
    g_run = false;
    if (g_th.joinable()) g_th.join();
    if (g_udp >= 0) close(g_udp);
    g_udp = -1;
}

}  // namespace onvif
