#include "game.h"
#include "storage.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <map>
#include <sstream>
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

namespace {
using Args = std::vector<std::string>;

std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> out;
    std::istringstream ss(line);
    std::string w;
    while (ss >> w) out.push_back(w);
    return out;
}

std::string upper(std::string s) {
    for (char& ch : s) ch = char(std::toupper(static_cast<unsigned char>(ch)));
    return s;
}
}  // namespace

class Protocol : public NetHandler {
public:
    Protocol(Network& net, Storage& storage) : net_(net), storage_(storage) {}

    void onLine(int fd, const std::string& line) override {
        Args a = split(line);
        if (a.empty()) return;
        const std::string cmd = upper(a[0]);
        if (cmd == "REGISTER") return cmdRegister(fd, a);
        if (cmd == "LOGIN") return cmdLogin(fd, a);
        if (cmd == "QUIT") { net_.send(fd, "BYE"); net_.closeAfterFlush(fd); return; }

        auto it = userByFd_.find(fd);
        if (cmd != "STATS") return net_.send(fd, "ERROR unknown_cmd");
        if (it == userByFd_.end()) return net_.send(fd, "ERROR not_logged_in");
        cmdStats(fd, it->second, a);
    }

    void onDisconnect(int fd) override {
        auto it = userByFd_.find(fd);
        if (it == userByFd_.end()) return;
        const std::string u = it->second;
        userByFd_.erase(fd);
        fdByUser_.erase(u);
        log_event("DISCONNECT " + u);
    }

private:
    bool online(const std::string& u) const { return fdByUser_.count(u) > 0; }

    void cmdRegister(int fd, const Args& a) {
        if (a.size() != 3) return net_.send(fd, "ERROR usage: REGISTER user pass");
        if (!valid_username(a[1])) return net_.send(fd, "ERROR bad_username");
        if (!valid_password(a[2])) return net_.send(fd, "ERROR bad_password");
        if (!storage_.registerUser(a[1], a[2])) return net_.send(fd, "ERROR user_exists");
        log_event("REGISTER " + a[1]);
        net_.send(fd, "OK REGISTER " + a[1]);
    }

    void cmdLogin(int fd, const Args& a) {
        if (userByFd_.count(fd)) return net_.send(fd, "ERROR already_logged_in");
        if (a.size() != 3) return net_.send(fd, "ERROR usage: LOGIN user pass");
        const std::string& u = a[1];
        if (!storage_.checkLogin(u, a[2])) return net_.send(fd, "ERROR bad_credentials");
        if (online(u)) return net_.send(fd, "ERROR already_online");
        userByFd_[fd] = u;
        fdByUser_[u] = fd;
        log_event("LOGIN " + u);
        net_.send(fd, "OK LOGIN " + u);
    }

    void cmdStats(int fd, const std::string& me, const Args& a) {
        if (a.size() > 2) return net_.send(fd, "ERROR usage: STATS [user]");
        const std::string target = a.size() == 2 ? a[1] : me;
        UserStats s;
        if (!storage_.getStats(target, s)) return net_.send(fd, "ERROR user_not_found");
        net_.send(fd, "STATS_INFO " + target + " " + std::to_string(s.wins) + " " +
                          std::to_string(s.losses) + " " + std::to_string(s.draws));
    }

    Network& net_;
    Storage& storage_;
    std::map<int, std::string> userByFd_;
    std::map<std::string, int> fdByUser_;
};

int main(int argc, char** argv) {
    int port = argc >= 2 ? std::stoi(argv[1]) : 5555;
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    std::signal(SIGPIPE, SIG_IGN);

    Network net;
    if (!net.listen(port)) return 1;

    Storage storage("users.txt");
    Protocol protocol(net, storage);
    log_event("SERVER_START port=" + std::to_string(port));

    std::cout << "Server listening on port " << port << "\n";
    net.run(protocol, g_stop);
    net.closeAll(protocol);
    return 0;
}