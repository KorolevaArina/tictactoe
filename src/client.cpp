#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>


static volatile std::sig_atomic_t g_stop = 0;


static void onSignal(int) {
    g_stop = 1;
}


struct ClientState {
    int sockFd = -1;
    bool run = true;
    bool quitSent = false;
    std::string myName;
};


static bool sendAll(int fd, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += size_t(n);
        }
        else if (n < 0 && errno == EINTR) {
            continue;
        }
        else {
            return false;
        }
    }
    return true;
}


static bool sendLine(int fd, const std::string& line) {
    return sendAll(fd, line + "\n");
}


static void handleServerLine(ClientState& st, const std::string& ln) {
    if (ln == "BYE") {
        std::cout << "До свидания!\n";
        st.run = false;
        return;
    }
    std::cout << "[srv] " << ln << "\n";
}


static void processUserLine(ClientState& st, const std::string& line) {
    if (line.empty()) {
        return;
    }
    sendLine(st.sockFd, line);
    std::istringstream ss(line);
    std::string cmd;
    ss >> cmd;
    if (cmd == "QUIT" || cmd == "quit") {
        st.quitSent = true;
    }
}


int main(int argc, char** argv) {
    std::signal(SIGINT, onSignal);
    std::signal(SIGPIPE, SIG_IGN);
    std::string host = argc >= 2 ? argv[1] : "127.0.0.1";
    int port = argc >= 3 ? std::stoi(argv[2]) : 5555;
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket"); 
        return 1; 
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(uint16_t(port));
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1 ||
        ::connect(fd, reinterpret_cast\<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("connect");
        ::close(fd);
        return 1;
    }

    ClientState st{ fd };
    std::cout << "Connected to " << host << ":" << port << "\n";
    std::string netAcc, stdinAcc;
    bool stdinEof = false;

    while (st.run && !g_stop) {
        std::vector\<pollfd> pfds{ {fd, POLLIN, 0} };
        if (!stdinEof) pfds.push_back({ STDIN_FILENO, POLLIN, 0 });
        int ret = ::poll(pfds.data(), nfds_t(pfds.size()), 100);
        if (ret <= 0) { if (ret < 0 && errno != EINTR) break; continue; }

        if (pfds[0].revents & (POLLIN | POLLHUP)) {
            char buf[4096];
            ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) {
                break;
            }
            netAcc.append(buf, size_t(n));
            size_t pos;
            while (st.run && (pos = netAcc.find('\n')) != std::string::npos) {
                std::string ln = netAcc.substr(0, pos);
                netAcc.erase(0, pos + 1);
                if (!ln.empty() && ln.back() == '\r') {
                    ln.pop_back();
                }
                handleServerLine(st, ln);
            }
        }
        if (!stdinEof && pfds.size() > 1 && (pfds[1].revents & (POLLIN | POLLHUP))) {
            char buf[1024];
            ssize_t n = ::read(STDIN_FILENO, buf, sizeof(buf));
            if (n <= 0) {
                stdinEof = true;
                continue; 
            }
            stdinAcc.append(buf, size_t(n));
            size_t pos;
            while (st.run && (pos = stdinAcc.find('\n')) != std::string::npos) {
                std::string ln = stdinAcc.substr(0, pos);
                stdinAcc.erase(0, pos + 1);
                if (!ln.empty() && ln.back() == '\r') {
                    ln.pop_back();
                }
                processUserLine(st, ln);
            }
        }
    }
    ::close(fd);
    return 0;
}