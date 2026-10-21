#pragma once
#include <memory>
#include <string>

#include "core/config.h"

// TLS (mbedTLS): HTTPS / WSS for the web portal, and HTTPS / MQTT-TLS for outgoing integrations.
//
// Server side is non-blocking and driven by the event loop (see Conn): raw bytes from the socket go into feed(),
// decrypted data comes out; encrypt() turns application data into ciphertext in out().
// The certificate is a self-signed ECDSA P-256 one created on first start (or one you upload).
namespace tls {

bool init();       // load the certificate, or create it; false when TLS is unavailable
bool available();  // a server certificate is loaded
json info();       // {"available","subject","fingerprint","not_after","self_signed"}
std::string regenerate();                                              // new self-signed certificate ("" or an error)
std::string install(const std::string &cert_pem, const std::string &key_pem);  // upload your own ("" or an error)

class ServerImpl;
class Server {
public:
    Server();
    ~Server();
    // returns 0 ok, -1 failure, -2 peer closed
    int feed(const char *data, size_t n, std::string &plain);
    bool encrypt(const char *data, size_t n);  // false on failure
    bool handshaking() const;
    std::string rout;  // ciphertext waiting for the socket
    size_t roff = 0;

private:
    std::unique_ptr<ServerImpl> impl_;
};

// ---- blocking client (webhooks, EdgeAI, MQTT over TLS). The socket has send/receive timeouts set.
struct ClientOpts {
    std::string verify = "system";  // system (CA bundle) | pin (SHA-256 of the certificate) | none
    std::string pin;                // hex SHA-256 of the server certificate, for verify = pin
};

class ClientImpl;
class Client {
public:
    Client();
    ~Client();
    bool connect(int fd, const std::string &host, const ClientOpts &o, std::string &err);
    int write(const char *d, size_t n);  // bytes written or -1
    int read(char *d, size_t n);         // bytes read, 0 = closed, -1 = error, -2 = receive timeout
    void close();

private:
    std::unique_ptr<ClientImpl> impl_;
};

}  // namespace tls
