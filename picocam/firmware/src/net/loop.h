#pragma once
#include <sys/epoll.h>

#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "tls.h"

// Single-threaded epoll loop shared by the HTTP, WebSocket and RTSP servers. Other threads hand work to it
// with post(); everything that touches a connection runs on the loop thread.
class EventLoop {
public:
    struct Handler {
        virtual ~Handler() {}
        virtual void on_event(uint32_t events) = 0;
    };

    bool init();
    void run();  // blocks until stop()
    void stop();

    void add(int fd, uint32_t events, Handler *h);
    void mod(int fd, uint32_t events, Handler *h);
    void del(int fd);

    void post(std::function<void()> fn);  // thread-safe
    // run fn every `ms` on the loop thread (coarse, checked once per wakeup)
    void every(int ms, std::function<void()> fn);

private:
    int ep_ = -1, efd_ = -1;
    std::atomic<bool> run_{false};
    std::mutex mu_;
    std::vector<std::function<void()>> posted_;
    struct Timer {
        int ms;
        int64_t next;
        std::function<void()> fn;
    };
    std::vector<Timer> timers_;
};

extern EventLoop g_loop;

// Buffered non-blocking TCP connection. Output is a queue of shared buffers so one encoded frame can be
// queued to many viewers without copies.
class Conn : public EventLoop::Handler, public std::enable_shared_from_this<Conn> {
public:
    explicit Conn(int fd, const std::string &peer);
    ~Conn() override;

    void start();  // register with the loop
    void send(std::shared_ptr<const std::string> buf);
    void send(std::string s) { send(std::make_shared<const std::string>(std::move(s))); }
    // stream a file region after the queued data (downloads, recordings)
    void send_file(const std::string &path, int64_t off, int64_t len);
    void close_after_send() { close_after_send_ = true; maybe_close(); }
    void close();

    // bytes queued for this peer (plain text not yet encrypted + ciphertext not yet sent)
    size_t pending() const { return pending_ + (tls_ ? tls_->rout.size() - tls_->roff : 0); }
    void enable_tls();  // server side: this connection speaks TLS
    bool is_tls() const { return tls_ != nullptr; }
    bool closed() const { return fd_ < 0; }
    const std::string &peer() const { return peer_; }
    int fd() const { return fd_; }

    // conns stay alive while registered here
    static std::map<int, std::shared_ptr<Conn>> &registry();
    // close connections that have been silent for idle_ms (slow-loris, half-open clients); conns that are sending
    // or declare themselves busy (WebSocket, playing RTSP, running async handler) are left alone
    static void sweep_idle(int64_t idle_ms);

protected:
    virtual void on_data(std::string &in) = 0;  // consume what you can from `in`
    virtual void on_close() {}
    virtual void on_drained() {}  // output queue became empty
    virtual bool idle_exempt() const { return false; }

    void on_event(uint32_t ev) override;
    void flush();
    void flush_tls();
    void flush_tail();
    void maybe_close();

    int fd_;
    std::string peer_;
    std::string in_;
    std::deque<std::shared_ptr<const std::string>> out_;
    size_t out_off_ = 0, pending_ = 0;
    bool want_write_ = false, close_after_send_ = false;
    int file_fd_ = -1;
    int64_t file_off_ = 0, file_left_ = 0;
    int64_t last_io_ = 0;
    std::unique_ptr<tls::Server> tls_;
};

// Listening socket that creates connections.
class Listener : public EventLoop::Handler {
public:
    using Factory = std::function<std::shared_ptr<Conn>(int fd, const std::string &peer)>;
    bool listen_tcp(int port, Factory f, bool tls = false);
    void close();
    int port() const { return port_; }
    void on_event(uint32_t ev) override;

private:
    int fd_ = -1, port_ = 0;
    Factory factory_;
    bool tls_ = false;
};
