#include "tls.h"

#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <mutex>

#include "core/log.h"
#include "core/util.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/ecp.h"
#include "mbedtls/error.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

namespace tls {

static const char *TAG = "tls";

// Randomness straight from the kernel: thread-safe (the server, the integrations and the HTTP workers all use mbedTLS)
static int rng(void *, unsigned char *out, size_t n) {
    std::string r = random_bytes(n);
    memcpy(out, r.data(), n);
    return 0;
}

static std::string err_text(int ret) {
    char buf[160];
    mbedtls_strerror(ret, buf, sizeof(buf));
    return buf;
}

static std::string hex_sha256(const unsigned char *p, size_t n) {
    unsigned char h[32];
    mbedtls_sha256(p, n, h, 0);
    return hex(h, 32);
}

static std::string cert_path() { return g_paths.data + "/tls/cert.pem"; }
static std::string key_path() { return g_paths.data + "/tls/key.pem"; }

// ---------------------------------------------------------------- server certificate and configuration

struct ServerConf {
    mbedtls_ssl_config conf;
    mbedtls_x509_crt crt;
    mbedtls_pk_context pk;
    ServerConf() {
        mbedtls_ssl_config_init(&conf);
        mbedtls_x509_crt_init(&crt);
        mbedtls_pk_init(&pk);
    }
    ~ServerConf() {
        mbedtls_ssl_config_free(&conf);
        mbedtls_x509_crt_free(&crt);
        mbedtls_pk_free(&pk);
    }
};

static std::mutex g_mu;
static std::shared_ptr<ServerConf> g_conf;

static std::string load_conf(const std::string &cert, const std::string &key, std::shared_ptr<ServerConf> &out) {
    auto sc = std::make_shared<ServerConf>();
    int r = mbedtls_x509_crt_parse(&sc->crt, (const unsigned char *)cert.c_str(), cert.size() + 1);
    if (r != 0) return "certificate: " + err_text(r);
    r = mbedtls_pk_parse_key(&sc->pk, (const unsigned char *)key.c_str(), key.size() + 1, nullptr, 0, rng, nullptr);
    if (r != 0) return "private key: " + err_text(r);
    if (mbedtls_pk_check_pair(&sc->crt.pk, &sc->pk, rng, nullptr) != 0) return "the private key does not belong to this certificate";
    r = mbedtls_ssl_config_defaults(&sc->conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (r != 0) return err_text(r);
    mbedtls_ssl_conf_rng(&sc->conf, rng, nullptr);
    mbedtls_ssl_conf_min_tls_version(&sc->conf, MBEDTLS_SSL_VERSION_TLS1_2);
    r = mbedtls_ssl_conf_own_cert(&sc->conf, &sc->crt, &sc->pk);
    if (r != 0) return err_text(r);
    out = sc;
    return "";
}

static std::string generate(std::string &cert_pem, std::string &key_pem) {
    mbedtls_pk_context key;
    mbedtls_pk_init(&key);
    mbedtls_x509write_cert crt;
    mbedtls_x509write_crt_init(&crt);
    std::string err;
    int r;
    do {
        if ((r = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY))) != 0) break;
        if ((r = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key), rng, nullptr)) != 0) break;
        std::string id = g_settings.get("device").value("id", "picocam");
        std::string host = g_settings.get("network").value("hostname", "picocam");
        mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
        mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
        mbedtls_x509write_crt_set_subject_key(&crt, &key);
        mbedtls_x509write_crt_set_issuer_key(&crt, &key);
        std::string name = "CN=" + id + ",O=PicoCam";
        if ((r = mbedtls_x509write_crt_set_subject_name(&crt, name.c_str())) != 0) break;
        if ((r = mbedtls_x509write_crt_set_issuer_name(&crt, name.c_str())) != 0) break;
        unsigned char serial[16];
        rng(nullptr, serial, sizeof(serial));
        serial[0] &= 0x7f;
        if ((r = mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof(serial))) != 0) break;
        // the board may not know the date yet: a wide fixed validity avoids "not yet valid" in browsers
        if ((r = mbedtls_x509write_crt_set_validity(&crt, "20240101000000", "20491231235959")) != 0) break;
        mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1);
        mbedtls_x509write_crt_set_key_usage(&crt, MBEDTLS_X509_KU_DIGITAL_SIGNATURE);
        mbedtls_x509_san_list n1, n2;
        memset(&n1, 0, sizeof(n1));
        memset(&n2, 0, sizeof(n2));
        n1.node.type = MBEDTLS_X509_SAN_DNS_NAME;
        n1.node.san.unstructured_name.p = (unsigned char *)host.data();
        n1.node.san.unstructured_name.len = host.size();
        n1.next = &n2;
        n2.node.type = MBEDTLS_X509_SAN_DNS_NAME;
        n2.node.san.unstructured_name.p = (unsigned char *)id.data();
        n2.node.san.unstructured_name.len = id.size();
        if ((r = mbedtls_x509write_crt_set_subject_alternative_name(&crt, &n1)) != 0) break;
        std::string buf(4096, '\0');
        if ((r = mbedtls_x509write_crt_pem(&crt, (unsigned char *)&buf[0], buf.size(), rng, nullptr)) != 0) break;
        cert_pem = buf.c_str();
        std::string kb(2048, '\0');
        if ((r = mbedtls_pk_write_key_pem(&key, (unsigned char *)&kb[0], kb.size())) != 0) break;
        key_pem = kb.c_str();
        r = 0;
    } while (false);
    if (r != 0) err = err_text(r);
    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&key);
    return err;
}

static std::string store_and_load(const std::string &cert, const std::string &key) {
    std::shared_ptr<ServerConf> sc;
    std::string e = load_conf(cert, key, sc);
    if (!e.empty()) return e;
    mkdirs(g_paths.data + "/tls");
    if (!write_file(key_path(), key) || !write_file(cert_path(), cert)) return "cannot store the certificate";
    std::lock_guard<std::mutex> lk(g_mu);
    g_conf = sc;
    return "";
}

bool init() {
    std::string cert, key;
    if (read_file(cert_path(), cert) && read_file(key_path(), key)) {
        std::shared_ptr<ServerConf> sc;
        std::string e = load_conf(cert, key, sc);
        if (e.empty()) {
            std::lock_guard<std::mutex> lk(g_mu);
            g_conf = sc;
            return true;
        }
        LOGW(TAG, "stored certificate unusable (%s): creating a new one", e.c_str());
    }
    return regenerate().empty();
}

std::string regenerate() {
    std::string cert, key;
    int64_t t0 = now_ms();
    std::string e = generate(cert, key);
    if (!e.empty()) {
        LOGE(TAG, "certificate generation failed: %s", e.c_str());
        return e;
    }
    e = store_and_load(cert, key);
    if (e.empty()) LOGI(TAG, "self-signed certificate created (%lld ms)", (long long)(now_ms() - t0));
    return e;
}

std::string install(const std::string &cert_pem, const std::string &key_pem) { return store_and_load(cert_pem, key_pem); }

bool available() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_conf != nullptr;
}

json info() {
    std::shared_ptr<ServerConf> sc;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        sc = g_conf;
    }
    if (!sc) return {{"available", false}};
    char subj[256] = {0}, issuer[256] = {0};
    mbedtls_x509_dn_gets(subj, sizeof(subj), &sc->crt.subject);
    mbedtls_x509_dn_gets(issuer, sizeof(issuer), &sc->crt.issuer);
    char na[32];
    snprintf(na, sizeof(na), "%04d-%02d-%02d", sc->crt.valid_to.year, sc->crt.valid_to.mon, sc->crt.valid_to.day);
    return {{"available", true},
            {"subject", subj},
            {"issuer", issuer},
            {"fingerprint", hex_sha256(sc->crt.raw.p, sc->crt.raw.len)},
            {"not_after", na},
            {"self_signed", std::string(subj) == std::string(issuer)}};
}

// ---------------------------------------------------------------- server session

class ServerImpl {
public:
    ServerImpl(std::shared_ptr<ServerConf> c, Server *owner) : sc(std::move(c)), owner(owner) {
        mbedtls_ssl_init(&ssl);
        mbedtls_ssl_setup(&ssl, &sc->conf);
        mbedtls_ssl_set_bio(&ssl, this, send_cb, recv_cb, nullptr);
    }
    ~ServerImpl() { mbedtls_ssl_free(&ssl); }
    static int send_cb(void *ctx, const unsigned char *buf, size_t len) {
        auto *s = (ServerImpl *)ctx;
        s->owner->rout.append((const char *)buf, len);
        return (int)len;
    }
    static int recv_cb(void *ctx, unsigned char *buf, size_t len) {
        auto *s = (ServerImpl *)ctx;
        if (s->rin.empty()) return MBEDTLS_ERR_SSL_WANT_READ;
        size_t n = std::min(len, s->rin.size());
        memcpy(buf, s->rin.data(), n);
        s->rin.erase(0, n);
        return (int)n;
    }
    std::shared_ptr<ServerConf> sc;
    Server *owner;
    mbedtls_ssl_context ssl;
    std::string rin;
    bool done = false;
};

Server::Server() {
    std::shared_ptr<ServerConf> c;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        c = g_conf;
    }
    if (c) impl_.reset(new ServerImpl(c, this));
}
Server::~Server() {}

bool Server::handshaking() const { return impl_ && !impl_->done; }

int Server::feed(const char *data, size_t n, std::string &plain) {
    if (!impl_) return -1;
    ServerImpl &s = *impl_;
    s.rin.append(data, n);
    if (!s.done) {
        int r = mbedtls_ssl_handshake(&s.ssl);
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) return 0;
        if (r != 0) return -1;
        s.done = true;
    }
    unsigned char buf[8192];
    for (;;) {
        int r = mbedtls_ssl_read(&s.ssl, buf, sizeof(buf));
        if (r > 0) {
            plain.append((const char *)buf, r);
            continue;
        }
        if (r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) return 0;
        if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return -2;
        return -1;
    }
}

bool Server::encrypt(const char *data, size_t n) {
    if (!impl_ || !impl_->done) return false;
    size_t off = 0;
    while (off < n) {
        int r = mbedtls_ssl_write(&impl_->ssl, (const unsigned char *)data + off, n - off);
        if (r > 0) off += r;
        else if (r == MBEDTLS_ERR_SSL_WANT_WRITE || r == MBEDTLS_ERR_SSL_WANT_READ) continue;
        else return false;
    }
    return true;
}

// ---------------------------------------------------------------- client

static std::mutex g_ca_mu;
static mbedtls_x509_crt g_ca;
static bool g_ca_loaded = false, g_ca_ok = false;

static bool load_ca() {
    std::lock_guard<std::mutex> lk(g_ca_mu);
    if (g_ca_loaded) return g_ca_ok;
    g_ca_loaded = true;
    mbedtls_x509_crt_init(&g_ca);
    for (const std::string &p : {g_paths.app + "/ca-bundle.pem", std::string("/etc/ssl/certs/ca-certificates.crt")}) {
        if (!file_exists(p)) continue;
        int r = mbedtls_x509_crt_parse_file(&g_ca, p.c_str());
        if (r >= 0) {  // > 0 = some certificates could not be parsed
            g_ca_ok = true;
            LOGI(TAG, "CA bundle loaded from %s", p.c_str());
            break;
        }
    }
    return g_ca_ok;
}

class ClientImpl {
public:
    ClientImpl() {
        mbedtls_ssl_init(&ssl);
        mbedtls_ssl_config_init(&conf);
    }
    ~ClientImpl() {
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&conf);
    }
    static int send_cb(void *ctx, const unsigned char *buf, size_t len) {
        ssize_t n = ::send(*(int *)ctx, buf, len, MSG_NOSIGNAL);
        return n < 0 ? -1 : (int)n;
    }
    static int recv_cb(void *ctx, unsigned char *buf, size_t len) {
        ssize_t n = ::recv(*(int *)ctx, buf, len, 0);  // blocking with a receive timeout
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return MBEDTLS_ERR_SSL_WANT_READ;  // timeout
        return n < 0 ? -1 : (int)n;
    }
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    int fd = -1;
    bool up = false;
};

Client::Client() : impl_(new ClientImpl()) {}
Client::~Client() { close(); }

bool Client::connect(int fd, const std::string &host, const ClientOpts &o, std::string &err) {
    ClientImpl &c = *impl_;
    c.fd = fd;
    int r = mbedtls_ssl_config_defaults(&c.conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (r != 0) return err = err_text(r), false;
    mbedtls_ssl_conf_rng(&c.conf, rng, nullptr);
    mbedtls_ssl_conf_min_tls_version(&c.conf, MBEDTLS_SSL_VERSION_TLS1_2);
    if (o.verify == "system") {
        if (!load_ca()) return err = "no CA bundle on the camera: pin the server certificate or turn verification off", false;
        mbedtls_ssl_conf_authmode(&c.conf, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&c.conf, &g_ca, nullptr);
    } else {
        mbedtls_ssl_conf_authmode(&c.conf, MBEDTLS_SSL_VERIFY_NONE);
    }
    if ((r = mbedtls_ssl_setup(&c.ssl, &c.conf)) != 0) return err = err_text(r), false;
    mbedtls_ssl_set_hostname(&c.ssl, host.c_str());
    mbedtls_ssl_set_bio(&c.ssl, &c.fd, ClientImpl::send_cb, ClientImpl::recv_cb, nullptr);
    int64_t t0 = now_ms();
    while ((r = mbedtls_ssl_handshake(&c.ssl)) != 0) {
        if ((r == MBEDTLS_ERR_SSL_WANT_READ || r == MBEDTLS_ERR_SSL_WANT_WRITE) && now_ms() - t0 < 20000) continue;
        if (r == MBEDTLS_ERR_SSL_WANT_READ) return err = "TLS handshake timed out", false;
        err = "TLS handshake failed: " + err_text(r);
        if (r == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
            char vb[256];
            mbedtls_x509_crt_verify_info(vb, sizeof(vb), "", mbedtls_ssl_get_verify_result(&c.ssl));
            err = "certificate not trusted: " + trim(vb) + " (is the camera clock right?)";
        }
        return false;
    }
    if (o.verify == "pin") {
        const mbedtls_x509_crt *peer = mbedtls_ssl_get_peer_cert(&c.ssl);
        std::string fp = peer ? hex_sha256(peer->raw.p, peer->raw.len) : "";
        std::string want = to_lower(o.pin);
        want.erase(std::remove(want.begin(), want.end(), ':'), want.end());
        if (fp.empty() || fp != want) return err = "server certificate does not match the pinned fingerprint (" + fp.substr(0, 16) + "…)", false;
    }
    c.up = true;
    return true;
}

int Client::write(const char *d, size_t n) {
    size_t off = 0;
    while (off < n) {
        int r = mbedtls_ssl_write(&impl_->ssl, (const unsigned char *)d + off, n - off);
        if (r > 0) off += r;
        else if (r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        else return -1;
    }
    return (int)n;
}

int Client::read(char *d, size_t n) {
    for (;;) {
        int r = mbedtls_ssl_read(&impl_->ssl, (unsigned char *)d, n);
        if (r > 0) return r;
        if (r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (r == MBEDTLS_ERR_SSL_WANT_READ) return -2;  // receive timeout
        if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || r == MBEDTLS_ERR_SSL_CONN_EOF) return 0;
        return -1;
    }
}

void Client::close() {
    if (impl_ && impl_->up) {
        mbedtls_ssl_close_notify(&impl_->ssl);
        impl_->up = false;
    }
}

}  // namespace tls
