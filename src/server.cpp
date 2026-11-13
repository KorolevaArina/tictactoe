// server.cpp (v1 — Network + StubProtocol + main)
#include "game.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

static volatile std::sig_atomic_t g_stop = 0;
static void onSignal(int) { g_stop = 1; }

static bool setNonBlocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

class NetHandler {
public:
    virtual ~NetHandler() = default;
    virtual void onLine(int fd, const std::string& line) = 0;
    virtual void onDisconnect(int fd) = 0;
};

struct Connection {
    int fd = -1;
    std::string readBuf;
    std::string writeBuf;
    bool dead = false;
    bool closing = false;
};

class Network {
public:
    static constexpr size_t kMaxLine = 64 * 1024;
    static constexpr size_t kMaxWriteBuf = 1024 * 1024;

    ~Network() {
        for (auto& [fd, c] : conns_) ::close(fd);
        if (listenFd_ >= 0) ::close(listenFd_);
    }

    bool listen(int port) {
        listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listenFd_ < 0) { std::perror("socket"); return false; }
        int opt = 1;
        ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(uint16_t(port));
        if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            std::perror("bind");
            return false;
        }
        if (::listen(listenFd_, 64) < 0) { std::perror("listen"); return false; }
        return setNonBlocking(listenFd_);
    }

    void run(NetHandler& handler, const volatile std::sig_atomic_t& stop) {
        while (!stop) {
            fd_set rfds, wfds;
            FD_ZERO(&rfds);
            FD_ZERO(&wfds);
            FD_SET(listenFd_, &rfds);
            int maxFd = listenFd_;
            for (auto& [fd, c] : conns_) {
                if (!c.closing) FD_SET(fd, &rfds);
                if (!c.writeBuf.empty()) FD_SET(fd, &wfds);
                if (fd > maxFd) maxFd = fd;
            }

            timeval tv{1, 0};  // TODO: heartbeat PING/PONG
            int ret = ::select(maxFd + 1, &rfds, &wfds, nullptr, &tv);
            if (ret < 0) {
                if (errno == EINTR) continue;
                std::perror("select");
                break;
            }
            if (ret > 0) {
                for (auto& [fd, c] : conns_) {
                    if (!c.dead && !c.closing && FD_ISSET(fd, &rfds)) readFrom(c, handler);
                    if (!c.dead && FD_ISSET(fd, &wfds)) flush(c);
                }
                if (FD_ISSET(listenFd_, &rfds)) acceptClients();
            }
            reap(handler);
        }
    }

    void send(int fd, const std::string& line) {
        auto it = conns_.find(fd);
        if (it == conns_.end() || it->second.dead) return;
        Connection& c = it->second;
        if (c.writeBuf.size() + line.size() + 1 > kMaxWriteBuf) {
            c.dead = true;
            return;
        }
        c.writeBuf += line;
        c.writeBuf += '\n';
    }

    void closeAfterFlush(int fd) {
        auto it = conns_.find(fd);
        if (it != conns_.end()) it->second.closing = true;
    }

    void closeAll(NetHandler& handler) {
        for (auto& [fd, c] : conns_) {
            handler.onDisconnect(fd);
            ::close(fd);
        }
        conns_.clear();
    }

private:
    void acceptClients() {
        while (true) {
            int fd = ::accept(listenFd_, nullptr, nullptr);
            if (fd < 0) {
                if (errno == EINTR) continue;
                return;
            }
            if (fd >= FD_SETSIZE || !setNonBlocking(fd)) {
                ::close(fd);
                continue;
            }
            Connection c;
            c.fd = fd;
            conns_[fd] = std::move(c);
        }
    }

    void readFrom(Connection& c, NetHandler& handler) {
        char buf[4096];
        while (true) {
            ssize_t n = ::recv(c.fd, buf, sizeof(buf), 0);
            if (n > 0) { c.readBuf.append(buf, size_t(n)); continue; }
            if (n == 0) { c.dead = true; return; }
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            c.dead = true;
            return;
        }
        size_t pos;
        while (!c.dead && !c.closing && (pos = c.readBuf.find('\n')) != std::string::npos) {
            std::string line = c.readBuf.substr(0, pos);
            c.readBuf.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) handler.onLine(c.fd, line);
        }
        if (c.readBuf.size() > kMaxLine) c.dead = true;
    }

    void flush(Connection& c) {
        while (!c.writeBuf.empty()) {
            ssize_t n = ::send(c.fd, c.writeBuf.data(), c.writeBuf.size(), MSG_NOSIGNAL);
            if (n > 0) c.writeBuf.erase(0, size_t(n));
            else if (n < 0 && errno == EINTR) continue;
            else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
            else { c.dead = true; return; }
        }
    }

    void reap(NetHandler& handler) {
        std::vector<int> toClose;
        for (auto& [fd, c] : conns_) {
            if (c.dead || (c.closing && c.writeBuf.empty())) toClose.push_back(fd);
        }
        for (int fd : toClose) {
            handler.onDisconnect(fd);
            ::close(fd);
            conns_.erase(fd);
        }
    }

    int listenFd_ = -1;
    std::map<int, Connection> conns_;
};

class StubProtocol : public NetHandler {
public:
    explicit StubProtocol(Network& net) : net_(net) {}
    void onLine(int fd, const std::string& line) override {
        if (line == "QUIT") {
            net_.send(fd, "BYE");
            net_.closeAfterFlush(fd);
            return;
        }
        net_.send(fd, "ERROR unknown_cmd");
    }
    void onDisconnect(int) override {}
private:
    Network& net_;
};

int main(int argc, char** argv) {
    int port = argc >= 2 ? std::stoi(argv[1]) : 5555;
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    std::signal(SIGPIPE, SIG_IGN);

    Network net;
    if (!net.listen(port)) return 1;
    StubProtocol protocol(net);
    std::cout << "Server listening on port " << port << "\n";
    net.run(protocol, g_stop);
    net.closeAll(protocol);
    return 0;
}