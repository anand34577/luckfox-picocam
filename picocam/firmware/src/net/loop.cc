#include "loop.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/eventfd.h>
#include <sys/sendfile.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "core/log.h"
#include "core/util.h"
#include "ipfilter.h"

EventLoop g_loop;
static const char *TAG = "net";

bool EventLoop::init() {
    ep_ = epoll_create1(EPOLL_CLOEXEC);
    efd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (ep_ < 0 || efd_ < 0) return false;
    epoll_event e{};
    e.events = EPOLLIN;
    e.data.ptr = nullptr;  // null = the wakeup eventfd
    epoll_ctl(ep_, EPOLL_CTL_ADD, efd_, &e);
    return true;
}

void EventLoop::add(int fd, uint32_t events, Handler *h) {
    epoll_event e{};
    e.events = events;
    e.data.ptr = h;
    epoll_ctl(ep_, EPOLL_CTL_ADD, fd, &e);
}

void EventLoop::mod(int fd, uint32_t events, Handler *h) {
    epoll_event e{};
    e.events = events;
    e.data.ptr = h;
    epoll_ctl(ep_, EPOLL_CTL_MOD, fd, &e);
}

void EventLoop::del(int fd) { epoll_ctl(ep_, EPOLL_CTL_DEL, fd, nullptr); }

void EventLoop::post(std::function<void()> fn) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        posted_.push_back(std::move(fn));
    }
    uint64_t one = 1;
    ssize_t r = write(efd_, &one, sizeof(one));
    (void)r;
}

void EventLoop::every(int ms, std::function<void()> fn) { timers_.push_back({ms, now_ms() + ms, std::move(fn)}); }

// async-signal-safe: an atomic store and a write()
void EventLoop::stop() {
    run_ = false;
    uint64_t one = 1;
    ssize_t r = write(efd_, &one, sizeof(one));
    (void)r;
}

void EventLoop::run() {
    run_ = true;
    epoll_event evs[64];
    while (run_) {
        int timeout = 1000;
        int64_t t = now_ms();
        for (auto &tm : timers_) timeout = std::min<int64_t>(timeout, std::max<int64_t>(0, tm.next - t));
        int n = epoll_wait(ep_, evs, 64, timeout);
        for (int i = 0; i < n; ++i) {
            if (!evs[i].data.ptr) {
                uint64_t v;
                ssize_t r = read(efd_, &v, sizeof(v));
                (void)r;
                continue;
            }
            ((Handler *)evs[i].data.ptr)->on_event(evs[i].events);
        }
        std::vector<std::function<void()>> todo;
        {
            std::lock_guard<std::mutex> lk(mu_);
            todo.swap(posted_);
        }
        for (auto &f : todo) f();
        t = now_ms();
        for (auto &tm : timers_)
            if (t >= tm.next) {
                tm.next = t + tm.ms;
                tm.fn();
            }
    }
}

// ---------------------------------------------------------------- Conn

std::map<int, std::shared_ptr<Conn>> &Conn::registry() {
    static std::map<int, std::shared_ptr<Conn>> r;
    return r;
}

Conn::Conn(int fd, const std::string &peer) : fd_(fd), peer_(peer), last_io_(now_ms()) {}

void Conn::sweep_idle(int64_t idle_ms) {
    std::vector<std::shared_ptr<Conn>> victims;
    int64_t t = now_ms();
    for (auto &kv : registry()) {
        Conn *c = kv.second.get();
        if (t - c->last_io_ > idle_ms && c->out_.empty() && c->file_fd_ < 0 && !c->idle_exempt()) victims.push_back(kv.second);
    }
    for (auto &c : victims) c->close();
}

Conn::~Conn() {
    if (file_fd_ >= 0) ::close(file_fd_);
    if (fd_ >= 0) ::close(fd_);
}

void Conn::enable_tls() { tls_.reset(new tls::Server()); }

void Conn::start() {
    registry()[fd_] = shared_from_this();
    g_loop.add(fd_, EPOLLIN | EPOLLRDHUP, this);
}

void Conn::close() {
    if (fd_ < 0) return;
    int fd = fd_;
    g_loop.del(fd);
    on_close();
    ::close(fd);
    fd_ = -1;
    out_.clear();
    pending_ = 0;
    // drop the registry reference last; `this` may be destroyed when the caller's reference goes
    auto self = shared_from_this();
    registry().erase(fd);
    g_loop.post([self] {});  // keep alive until the current callback stack unwinds
}

void Conn::send(std::shared_ptr<const std::string> buf) {
    if (fd_ < 0 || !buf || buf->empty()) return;
    pending_ += buf->size();
    out_.push_back(std::move(buf));
    flush();
}

void Conn::send_file(const std::string &path, int64_t off, int64_t len) {
    if (fd_ < 0) return;
    if (file_fd_ >= 0) ::close(file_fd_);
    file_fd_ = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    file_off_ = off;
    file_left_ = file_fd_ >= 0 ? len : 0;
    flush();
}

void Conn::flush() {
    if (fd_ < 0) return;
    if (tls_) {
        flush_tls();
        return;
    }
    while (!out_.empty()) {
        iovec iov[16];
        int n = 0;
        size_t off = out_off_;
        for (auto it = out_.begin(); it != out_.end() && n < 16; ++it, ++n) {
            iov[n].iov_base = (void *)((*it)->data() + off);
            iov[n].iov_len = (*it)->size() - off;
            off = 0;
        }
        ssize_t w = writev(fd_, iov, n);
        if (w < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            close();
            return;
        }
        pending_ -= w;
        last_io_ = now_ms();
        size_t left = (size_t)w;
        while (left > 0 && !out_.empty()) {
            size_t avail = out_.front()->size() - out_off_;
            if (left >= avail) {
                left -= avail;
                out_.pop_front();
                out_off_ = 0;
            } else {
                out_off_ += left;
                left = 0;
            }
        }
    }
    // file body goes after all queued buffers
    while (out_.empty() && file_fd_ >= 0 && file_left_ > 0) {
        off_t o = (off_t)file_off_;
        ssize_t w = sendfile(fd_, file_fd_, &o, (size_t)std::min<int64_t>(file_left_, 256 * 1024));
        if (w < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            close();
            return;
        }
        if (w == 0) file_left_ = 0;
        file_off_ += w;
        file_left_ -= w;
    }
    flush_tail();
}

// TLS: ciphertext waits in tls_->rout; plain buffers and files are encrypted as the socket takes data
void Conn::flush_tls() {
    std::string &ro = tls_->rout;
    size_t &off = tls_->roff;
    bool blocked = false;
    while (!blocked) {
        while (off < ro.size()) {
            ssize_t w = ::send(fd_, ro.data() + off, ro.size() - off, MSG_NOSIGNAL);
            if (w < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    blocked = true;
                    break;
                }
                close();
                return;
            }
            off += (size_t)w;
            last_io_ = now_ms();
        }
        if (blocked) break;
        ro.clear();
        off = 0;
        if (!out_.empty()) {
            const std::string &front = *out_.front();
            size_t n = std::min<size_t>(front.size() - out_off_, 64 * 1024);
            if (!tls_->encrypt(front.data() + out_off_, n)) {
                close();
                return;
            }
            pending_ -= n;
            out_off_ += n;
            if (out_off_ >= front.size()) {
                out_.pop_front();
                out_off_ = 0;
            }
        } else if (file_fd_ >= 0 && file_left_ > 0) {
            char buf[32768];
            ssize_t n = pread(file_fd_, buf, (size_t)std::min<int64_t>(sizeof(buf), file_left_), (off_t)file_off_);
            if (n <= 0) {
                file_left_ = 0;
            } else {
                if (!tls_->encrypt(buf, (size_t)n)) {
                    close();
                    return;
                }
                file_off_ += n;
                file_left_ -= n;
            }
        } else {
            break;
        }
    }
    flush_tail();
}

void Conn::flush_tail() {
    if (file_fd_ >= 0 && file_left_ <= 0) {
        ::close(file_fd_);
        file_fd_ = -1;
    }
    bool need = !out_.empty() || file_fd_ >= 0 || (tls_ && tls_->roff < tls_->rout.size());
    if (need != want_write_) {
        want_write_ = need;
        g_loop.mod(fd_, EPOLLIN | EPOLLRDHUP | (need ? EPOLLOUT : 0), this);
    }
    if (!need) {
        on_drained();
        maybe_close();
    }
}

void Conn::maybe_close() {
    if (close_after_send_ && out_.empty() && file_fd_ < 0 && (!tls_ || tls_->roff >= tls_->rout.size())) close();
}

void Conn::on_event(uint32_t ev) {
    auto self = shared_from_this();  // a handler may close us
    if (ev & EPOLLIN) {
        char buf[16384];
        for (;;) {
            ssize_t r = recv(fd_, buf, sizeof(buf), 0);
            if (r > 0) {
                last_io_ = now_ms();
                if (tls_) {
                    int fr = tls_->feed(buf, (size_t)r, in_);  // decrypted data lands in in_
                    if (!tls_->rout.empty()) flush();            // handshake messages, alerts
                    if (fd_ < 0) return;
                    if (fr < 0) {
                        close();
                        return;
                    }
                } else {
                    in_.append(buf, r);
                }
                if (in_.size() > 64 * 1024) {
                    on_data(in_);
                    if (fd_ < 0) return;
                }
                continue;
            }
            if (r == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                if (!in_.empty()) on_data(in_);
                close();
                return;
            }
            break;
        }
        if (!in_.empty()) on_data(in_);
        if (fd_ < 0) return;
    }
    if (ev & (EPOLLHUP | EPOLLERR)) {
        close();
        return;
    }
    if (ev & EPOLLOUT) flush();
}

// ---------------------------------------------------------------- Listener

bool Listener::listen_tcp(int port, Factory f, bool tls) {
    tls_ = tls;
    fd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    int one = 1;
    setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = INADDR_ANY;
    if (bind(fd_, (sockaddr *)&a, sizeof(a)) < 0 || listen(fd_, 32) < 0) {
        LOGE(TAG, "cannot listen on port %d: %s", port, strerror(errno));
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    port_ = port;
    factory_ = std::move(f);
    g_loop.add(fd_, EPOLLIN, this);
    return true;
}

void Listener::close() {
    if (fd_ < 0) return;
    g_loop.del(fd_);
    ::close(fd_);
    fd_ = -1;
}

void Listener::on_event(uint32_t) {
    for (;;) {
        sockaddr_in a{};
        socklen_t al = sizeof(a);
        int c = accept4(fd_, (sockaddr *)&a, &al, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (c < 0) {
            // out of descriptors: the listener stays readable, so free idle ones now instead of spinning
            if (errno == EMFILE || errno == ENFILE) Conn::sweep_idle(3000);
            break;
        }
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &a.sin_addr, ip, sizeof(ip));
        // connection caps: total and per client address
        {
            size_t total = Conn::registry().size(), same = 0;
            for (auto &kv : Conn::registry()) same += kv.second->peer() == ip;
            if (!ipfilter::allowed(ip)) {
                ::close(c);
                continue;
            }
            if (total >= 96 || same >= 24) {
                LOGW(TAG, "too many connections (%zu total, %zu from %s): refused", total, same, ip);
                ::close(c);
                continue;
            }
        }
        int one = 1;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));  // latency over packet count
        auto conn = factory_(c, ip);
        if (conn && tls_) {
            if (!tls::available()) conn.reset();
            else conn->enable_tls();
        }
        if (conn) conn->start();
        else ::close(c);
    }
}
