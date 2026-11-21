#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <termios.h>

#include <cctype>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <deque>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

static volatile std::sig_atomic_t g_stop = 0;

static void onSignal(int) {
    g_stop = 1;
}

// ---------- Цвета (ANSI) ----------
namespace color {
static const char* kReset   = "\033[0m";
static const char* kBold    = "\033[1m";
static const char* kRed     = "\033[31m";
static const char* kGreen   = "\033[32m";
static const char* kYellow  = "\033[33m";
static const char* kCyan    = "\033[36m";
static const char* kGray    = "\033[90m";
static const char* kMagenta = "\033[35m";
}  // namespace color

// ---------- TerminalGuard: raw-режим для ввода пароля ----------
struct TerminalGuard {
    bool isTty = false;
    termios orig{};

    TerminalGuard() {
        isTty = (::isatty(STDIN_FILENO) == 1);
        if (isTty && ::tcgetattr(STDIN_FILENO, &orig) == 0) {
            termios raw = orig;
            raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
            raw.c_cc[VMIN] = 1;
            raw.c_cc[VTIME] = 0;
            ::tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        } else {
            isTty = false;
        }
    }

    ~TerminalGuard() {
        if (isTty) ::tcsetattr(STDIN_FILENO, TCSANOW, &orig);
    }
};

struct ClientState {
    int sockFd = -1;
    bool run = true;
    bool quitSent = false;
    std::string myName;

    // --- партия и ввод ---
    bool isTty = false;
    std::string oppName, lastBoard, inputBuf, pwCmd, pwUser, pwBuf;
    char mySide = '?', currentTurn = '?';
    int N = 3, K = 3, lastR = -1, lastC = -1, escState = 0;
    bool readingPassword = false;
    std::set<std::pair<int, int>> winCells;

    // --- чат ---
    std::deque<std::string> chatLog;
};

static bool sendAll(int fd, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += size_t(n);
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

static bool sendLine(int fd, const std::string& line) {
    return sendAll(fd, line + "\n");
}

static std::string trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// ---------- Отрисовка доски ----------
static void renderBoard(const ClientState& st) {
    if (st.lastBoard.empty() && st.chatLog.empty()) return;
    std::ostringstream out;
    out << "\n";
    for (int r = 0; r < st.N; ++r) {
        for (int c = 0; c < st.N; ++c) {
            out << (c == 0 ? " " : " ");
            if (!st.lastBoard.empty()) {
                // boardLine() приходит одной строкой; будем резать по N*N
            }
        }
        out << "\n";
    }
    if (!st.chatLog.empty()) {
        out << color::kGray << "--- чат ---" << color::kReset << "\n";
        for (const auto& m : st.chatLog) out << m << "\n";
    }
    std::cout << out.str() << std::flush;
}

// ---------- Разбор серверных строк ----------
static void handleServerLine(ClientState& st, const std::string& ln) {
    if (ln == "BYE") {
        std::cout << color::kCyan << "До свидания!" << color::kReset << "\n";
        st.run = false;
        return;
    }

    std::istringstream ss(ln);
    std::string tag;
    ss >> tag;

    if (tag == "OK") {
        std::string what, who;
        ss >> what >> who;
        if (what == "LOGIN") {
            st.myName = who;
            std::cout << color::kGreen << "Вы вошли как " << who << color::kReset << "\n";
            return;
        }
        if (what == "REGISTER") {
            std::cout << color::kGreen << "Зарегистрирован: " << who << color::kReset << "\n";
            return;
        }
        std::cout << "[srv] " << ln << "\n";
        return;
    }

    if (tag == "ERROR") {
        std::string code;
        ss >> code;
        std::cout << color::kRed << "Ошибка: " << code << color::kReset << "\n";
        return;
    }

    // --- GAME_START и RESUMED ---
    if (tag == "GAME_START" || tag == "RESUMED") {
        std::string opp, side;
        int n = 3, k = 3;
        ss >> opp >> side >> n >> k;
        st.oppName = opp;
        st.mySide = side.empty() ? '?' : side[0];
        st.N = n;
        st.K = k;
        st.lastR = st.lastC = -1;
        st.winCells.clear();
        st.chatLog.clear();
        std::cout << color::kCyan << color::kBold << "--- "
                  << (tag == "RESUMED" ? "Партия возобновлена: " : "Старт: ")
                  << color::kReset << color::kBold << opp
                  << " (" << n << "x" << n << ", K=" << k << ", вы — "
                  << st.mySide << ")" << color::kReset << "\n";
        return;
    }

    if (tag == "RESUMED_AVAILABLE") {
        std::string opp, turn;
        ss >> opp >> turn;
        std::cout << color::kYellow << color::kBold
                  << "Есть сохранённая партия против " << opp
                  << " (ход " << turn << "). Введите RESUME, чтобы продолжить."
                  << color::kReset << "\n";
        return;
    }

    if (tag == "WAIT_OPPONENT") {
        std::string opp;
        ss >> opp;
        std::cout << color::kCyan
                  << "Ждём соперника " << opp << " для продолжения партии..."
                  << color::kReset << "\n";
        return;
    }

    if (tag == "NO_SAVE") {
        std::cout << color::kGray << "Сохранённых партий нет." << color::kReset << "\n";
        return;
    }

    if (tag == "OPPONENT_LEFT_SAVED") {
        std::cout << color::kYellow << color::kBold
                  << "Соперник отключился, партия сохранена." << color::kReset << "\n";
        st.mySide = '?';
        st.currentTurn = '?';
        st.oppName.clear();
        return;
    }

    // --- чат ---
    if (tag == "CHAT_MSG") {
        std::string scope, from, rest;
        ss >> scope >> from;
        std::getline(ss, rest);
        rest = trim(rest);
        std::string formatted =
            std::string(scope == "game" ? color::kMagenta "[Игра] " : color::kCyan "[Лобби] ") +
            color::kBold + from + color::kReset + ": " + rest;
        if (scope == "game") {
            st.chatLog.push_back(formatted);
            while (st.chatLog.size() > 5) st.chatLog.pop_front();
        }
        std::cout << formatted << "\n";
        return;
    }

    // --- таблица лидеров ---
    if (tag == "TOP_BEGIN") {
        int count = 0;
        ss >> count;
        std::cout << color::kBold << "Таблица лидеров (топ-" << count << "):"
                  << color::kReset << "\n";
        return;
    }
    if (tag == "TOP_ENTRY") {
        int place = 0, elo = 0, w = 0, l = 0, d = 0;
        std::string name;
        ss >> place >> name >> elo >> w >> l >> d;
        std::cout << "  " << std::setw(2) << place << ". "
                  << color::kBold << name << color::kReset
                  << "  elo=" << color::kGreen << elo << color::kReset
                  << "  W/L/D=" << w << "/" << l << "/" << d << "\n";
        return;
    }
    if (tag == "TOP_END") return;

    if (tag == "TURN") {
        std::string side;
        ss >> side;
        st.currentTurn = side.empty() ? '?' : side[0];
        std::cout << (st.currentTurn == st.mySide
                          ? color::kGreen
                          : color::kGray)
                  << (st.currentTurn == st.mySide ? "Ваш ход" : "Ход соперника")
                  << color::kReset << "\n";
        return;
    }

    if (tag == "LAST") {
        ss >> st.lastR >> st.lastC;
        return;
    }

    if (tag == "WIN") {
        std::string side;
        ss >> side;
        std::string coords;
        std::getline(ss, coords);
        std::istringstream cs(coords);
        std::string cell;
        st.winCells.clear();
        while (std::getline(cs, cell, ';')) {
            if (cell.empty()) continue;
            auto comma = cell.find(',');
            if (comma == std::string::npos) continue;
            int r = std::stoi(cell.substr(0, comma));
            int c = std::stoi(cell.substr(comma + 1));
            st.winCells.insert({r, c});
        }
        std::cout << color::kBold
                  << (side.size() && side[0] == st.mySide ? "Вы победили!" : "Вы проиграли")
                  << color::kReset << "\n";
        return;
    }

    if (tag == "DRAW") {
        std::cout << color::kYellow << "Ничья" << color::kReset << "\n";
        return;
    }

    if (tag == "INVITE_FROM") {
        std::string from;
        int n = 3, k = 3;
        ss >> from >> n >> k;
        std::cout << color::kCyan << "Приглашение от " << from
                  << " (" << n << "x" << n << ", K=" << k << "). "
                  << "ACCEPT " << from << " / DECLINE " << from
                  << color::kReset << "\n";
        return;
    }

    if (tag == "INVITE_SENT" || tag == "INVITE_DECLINED" ||
        tag == "INVITE_CANCELED" || tag == "OK_DECLINED") {
        std::cout << "[srv] " << ln << "\n";
        return;
    }

    if (tag == "QUEUED") {
        std::cout << color::kCyan << "В очереди: " << ln << color::kReset << "\n";
        return;
    }

    if (tag == "WHO_BEGIN") {
        std::cout << color::kBold << "Игроки:" << color::kReset << "\n";
        return;
    }
    if (tag == "WHO_USER") {
        std::string u, status;
        ss >> u >> status;
        std::cout << "  " << u << " [" << status << "]\n";
        return;
    }
    if (tag == "WHO_END") return;

    if (tag == "STATS_INFO") {
        std::string u;
        int w = 0, l = 0, d = 0, elo = 0;
        ss >> u >> w >> l >> d >> elo;
        std::cout << color::kBold << "Статистика " << u << ": "
                  << color::kReset << "побед=" << w << " поражений=" << l
                  << " ничьих=" << d;
        if (elo > 0) std::cout << " elo=" << color::kGreen << elo << color::kReset;
        std::cout << "\n";
        return;
    }

    // Иначе — считаем, что это доска (boardLine).
    st.lastBoard = ln;
    std::cout << color::kGray << ln << color::kReset << "\n";
}

// ---------- Ввод пользователя ----------
static void processUserLine(ClientState& st, const std::string& line);

static void flushInput(ClientState& st) {
    size_t pos;
    while (st.run && (pos = st.inputBuf.find('\n')) != std::string::npos) {
        std::string ln = st.inputBuf.substr(0, pos);
        st.inputBuf.erase(0, pos + 1);
        if (!ln.empty() && ln.back() == '\r') ln.pop_back();

        if (st.readingPassword) {
            st.pwBuf = ln;
            st.readingPassword = false;
            std::string full = st.pwCmd + " " + st.pwUser + " " + st.pwBuf;
            sendLine(st.sockFd, full);
            std::cout << "\n";
            st.pwCmd.clear();
            st.pwUser.clear();
            st.pwBuf.clear();
        } else {
            processUserLine(st, ln);
        }
    }
}

static void processUserLine(ClientState& st, const std::string& line) {
    if (line.empty()) return;

    std::istringstream ss(line);
    std::string cmd;
    ss >> cmd;

    std::string up;
    for (char ch : cmd) up.push_back(char(std::toupper(static_cast<unsigned char>(ch))));

    // Перехватываем REGISTER/LOGIN, чтобы спросить пароль отдельно.
    if (up == "REGISTER" || up == "LOGIN") {
        std::string user, pass;
        ss >> user;
        ss >> pass;
        if (!user.empty() && !pass.empty()) {
            sendLine(st.sockFd, line);
            return;
        }
        if (user.empty()) {
            std::cout << color::kRed << "usage: " << up << " user pass" << color::kReset << "\n";
            return;
        }
        st.pwCmd = up;
        st.pwUser = user;
        st.pwBuf.clear();
        st.readingPassword = true;
        std::cout << "Пароль: " << std::flush;
        return;
    }

    sendLine(st.sockFd, line);
    if (up == "QUIT") st.quitSent = true;
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
        ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        perror("connect");
        ::close(fd);
        return 1;
    }

    TerminalGuard guard;

    ClientState st;
    st.sockFd = fd;
    st.isTty = guard.isTty;

    std::cout << "Connected to " << host << ":" << port << "\n";

    std::string netAcc, stdinAcc;
    bool stdinEof = false;

    while (st.run && !g_stop) {
        std::vector<pollfd> pfds{{fd, POLLIN, 0}};
        if (!stdinEof) pfds.push_back({STDIN_FILENO, POLLIN, 0});

        int ret = ::poll(pfds.data(), nfds_t(pfds.size()), 100);
        if (ret <= 0) {
            if (ret < 0 && errno != EINTR) break;
            continue;
        }

        if (pfds[0].revents & (POLLIN | POLLHUP)) {
            char buf[4096];
            ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0) break;
            netAcc.append(buf, size_t(n));
            size_t pos;
            while (st.run && (pos = netAcc.find('\n')) != std::string::npos) {
                std::string ln = netAcc.substr(0, pos);
                netAcc.erase(0, pos + 1);
                if (!ln.empty() && ln.back() == '\r') ln.pop_back();
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

            // В raw-режиме эхо выключено — нужно вручную отрисовать ввод.
            if (st.readingPassword && st.isTty) {
                for (ssize_t i = 0; i < n; ++i) {
                    char ch = buf[i];
                    if (ch == '\n' || ch == '\r') {
                        st.inputBuf.push_back('\n');
                    } else if (ch == 127 || ch == 8) {
                        if (!st.inputBuf.empty()) {
                            st.inputBuf.pop_back();
                            std::cout << "\b \b" << std::flush;
                        }
                    } else {
                        st.inputBuf.push_back(ch);
                        std::cout << '*' << std::flush;
                    }
                }
                flushInput(st);
            } else {
                st.inputBuf.append(stdinAcc);
                stdinAcc.clear();
                flushInput(st);
            }
        }
    }

    ::close(fd);
    return 0;
}