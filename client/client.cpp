#include <iostream>
#include <string>
#include <vector>
#include <deque>
#include <set>
#include <utility>
#include <sstream>
#include <iomanip>
#include <cmath>
#include <cstring>
#include <cctype>
#include <csignal>
#include <cerrno>

#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <poll.h>
#include <termios.h>

#define RESET "\033[0m"
#define RED "\033[31m"
#define GREEN "\033[32m"
#define YELLOW "\033[33m"
#define BLUE "\033[34m"
#define MAGENTA "\033[35m"
#define CYAN "\033[36m"
#define BOLD "\033[1m"
#define DIM "\033[2m"
#define BG_YELLOW "\033[43;30;1m"
#define BG_GREEN "\033[42;30;1m"

static volatile std::sig_atomic_t g_stop = 0;
static void onSignal(int) { g_stop = 1; }

struct TerminalGuard
{
    bool isTty = false;
    termios orig{};

    TerminalGuard()
    {
        isTty = (::isatty(STDIN_FILENO) == 1);
        if (isTty && ::tcgetattr(STDIN_FILENO, &orig) == 0)
        {
            termios raw = orig;
            raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
            raw.c_cc[VMIN] = 1;
            raw.c_cc[VTIME] = 0;
            ::tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        }
        else
        {
            isTty = false;
        }
    }

    ~TerminalGuard()
    {
        if (isTty)
        {
            ::tcsetattr(STDIN_FILENO, TCSANOW, &orig);
        }
    }
};

struct ClientState
{
    int sockFd = -1;
    bool isTty = false;
    bool run = true;
    bool quitSent = false;

    std::string myName;
    std::string oppName;
    char mySide = '?';
    char currentTurn = '?';
    int N = 3;
    int K = 3;

    std::string lastBoard;
    int lastR = -1;
    int lastC = -1;
    std::set<std::pair<int, int>> winCells;
    std::deque<std::string> chatLog;

    // Состояние ввода
    std::string inputBuf;
    int escState = 0;
    bool readingPassword = false;
    std::string pwCmd;
    std::string pwUser;
    std::string pwBuf;
};

static bool sendAll(int fd, const std::string &data)
{
    size_t sent = 0;
    while (sent < data.size())
    {
        ssize_t n = ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n > 0)
        {
            sent += static_cast<size_t>(n);
        }
        else if (n < 0 && errno == EINTR)
        {
            continue;
        }
        else
        {
            return false;
        }
    }
    return true;
}

static bool sendLine(int fd, const std::string &line)
{
    return sendAll(fd, line + "\n");
}

static std::string trim(const std::string &s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos)
        return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

static std::string upperFirstWord(const std::string &s)
{
    std::string out = s;
    size_t i = 0;
    while (i < out.size() && !std::isspace(static_cast<unsigned char>(out[i])))
    {
        out[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[i])));
        ++i;
    }
    return out;
}

static bool parseTwoInts(const std::string &s, int &r, int &c)
{
    std::istringstream ss(s);
    std::string a, b, extra;
    if (!(ss >> a >> b) || (ss >> extra))
        return false;
    auto isInt = [](const std::string &tok, int &val)
    {
        if (tok.empty())
            return false;
        size_t i = 0;
        if (tok[0] == '+' || tok[0] == '-')
        {
            i = 1;
            if (tok.size() == 1)
                return false;
        }
        for (; i < tok.size(); ++i)
            if (!std::isdigit(static_cast<unsigned char>(tok[i])))
                return false;
        try
        {
            val = std::stoi(tok);
        }
        catch (...)
        {
            return false;
        }
        return true;
    };
    return isInt(a, r) && isInt(b, c);
}

static void clearPromptLine(const ClientState &st)
{
    if (st.isTty)
    {
        std::cout << "\r\033[2K" << std::flush;
    }
}

static void drawPrompt(const ClientState &st)
{
    if (!st.isTty || !st.run)
        return;
    if (st.readingPassword)
    {
        std::cout << "\r\033[2K" << RESET << "Пароль для " << st.pwUser << ": "
                  << std::string(st.pwBuf.size(), '*') << std::flush;
    }
    else
    {
        std::cout << "\r\033[2K" << RESET << CYAN << "> " << RESET << st.inputBuf << std::flush;
    }
}

static void clearScreen()
{
    std::cout << "\033[0m\033[2J\033[H" << std::flush;
}

static void drawBoard(const ClientState &st)
{
    if (st.lastBoard.empty())
        return;
    int N = st.N;
    if (N < 1 || static_cast<int>(st.lastBoard.size()) != N * N)
        return;

    std::cout << RESET << "\n";
    if (!st.oppName.empty())
    {
        std::cout << BOLD << CYAN << "Партия: " << RESET << BOLD
                  << (st.myName.empty() ? "?" : st.myName) << " ("
                  << (st.mySide == 'X' ? RED : (st.mySide == 'O' ? BLUE : RESET))
                  << (st.mySide == '?' ? "?" : std::string(1, st.mySide)) << RESET << BOLD << ")"
                  << " vs " << st.oppName
                  << DIM << "  [" << N << "x" << N << ", " << st.K << " в ряд]" << RESET << "\n\n";
    }

    const std::string hline = "+---";
    std::cout << "   ";
    for (int c = 0; c < N; ++c)
        std::cout << " " << c << "  ";
    std::cout << "\n";

    for (int r = 0; r < N; ++r)
    {
        std::cout << "  ";
        for (int c = 0; c < N; ++c)
            std::cout << hline;
        std::cout << "+\n";
        std::cout << r << " ";
        for (int c = 0; c < N; ++c)
        {
            char ch = st.lastBoard[r * N + c];
            std::string sym = (ch == '_' ? " " : std::string(1, ch));
            bool isWin = st.winCells.count({r, c}) > 0;
            bool isLast = (r == st.lastR && c == st.lastC);

            std::cout << RESET << "|";
            if (isWin)
            {
                std::cout << BG_GREEN << " " << sym << " " << RESET;
            }
            else if (isLast && ch != '_')
            {
                std::cout << BG_YELLOW << " " << sym << " " << RESET;
            }
            else
            {
                std::string color = (ch == 'X' ? RED : (ch == 'O' ? BLUE : RESET));
                std::cout << " " << color << BOLD << sym << RESET << " ";
            }
        }
        std::cout << RESET << "|\n";
    }
    std::cout << "  ";
    for (int c = 0; c < N; ++c)
        std::cout << hline;
    std::cout << "+\n";

    if (st.mySide != '?' && st.winCells.empty())
    {
        std::cout << BOLD << "[" << (st.myName.empty() ? "?" : st.myName) << " "
                  << st.mySide << " | N=" << st.N << " K=" << st.K << "]" << RESET;
        if (st.currentTurn == 'X' || st.currentTurn == 'O')
        {
            std::cout << "  >>> Ход: "
                      << (st.currentTurn == 'X' ? RED : BLUE) << BOLD << st.currentTurn << RESET;
            if (st.currentTurn == st.mySide)
                std::cout << GREEN << " (ваш ход! введите: r c)" << RESET;
            else
                std::cout << DIM << " (ждём соперника)" << RESET;
        }
        std::cout << "\n";
    }

    if (!st.chatLog.empty())
    {
        std::cout << DIM << "--- Чат ---\n"
                  << RESET;
        for (const auto &msg : st.chatLog)
        {
            std::cout << "  " << msg << "\n";
        }
    }
}

static void printHelp()
{
    std::cout << BOLD << "Команды:\n"
              << RESET << "  REGISTER <user> [password]    — регистрация\n"
                          "  LOGIN    <user> [password]    — вход в аккаунт\n"
                          "  WHO                           — список игроков онлайн\n"
                          "  TOP                           — таблица лидеров (Топ-5 Elo)\n"
                          "  STATS    [user]               — подробная статистика игрока\n"
                          "  NEWGAME  [N K]                — найти соперника (очередь), поле NxN, K в ряд\n"
                          "  NEWGAME  [N K] <opp>          — пригласить игрока (3<=N<=10, 3<=K<=N; по умолч. 3 3)\n"
                          "  ACCEPT   [user]               — принять приглашение\n"
                          "  DECLINE  [user]               — отклонить приглашение\n"
                          "  CANCEL                        — отменить приглашения / выйти из очереди\n"
                          "  MOVE     <row> <col>          — сделать ход (в игре можно просто: <row> <col>)\n"
                          "  CHAT     <сообщение>          — чат (в игре — сопернику, в лобби — всем)\n"
                          "  RESUME                        — возобновить сохранённую партию\n"
                          "  CLEAR                         — очистить экран\n"
                          "  QUIT                          — выход\n";
}

static void handleServerLine(ClientState &st, const std::string &ln)
{
    if (ln == "PING")
    {
        sendLine(st.sockFd, "PONG");
        return;
    }

    clearPromptLine(st);

    std::istringstream ss(ln);
    std::string tag;
    ss >> tag;

    if (tag == "BOARD")
    {
        // "BOARD X O _ ..." — N*N символов через пробел; N = sqrt(количества).
        std::string cells, tok;
        while (ss >> tok)
        {
            if (tok.size() == 1)
                cells += tok[0];
        }
        int N = static_cast<int>(std::lround(std::sqrt(static_cast<double>(cells.size()))));
        if (N > 0 && N * N == static_cast<int>(cells.size()))
        {
            st.N = N;
            st.lastBoard = cells;
            st.winCells.clear();
            clearScreen();
            drawBoard(st);
        }
    }
    else if (tag == "LAST")
    {
        ss >> st.lastR >> st.lastC;
    }
    else if (tag == "TURN")
    {
        std::string side;
        ss >> side;
        st.currentTurn = side.empty() ? '?' : side[0];
    }
    else if (tag == "WIN")
    {
        std::string side, coords;
        ss >> side >> coords;
        st.winCells.clear();
        if (!coords.empty())
        {
            std::istringstream cs(coords);
            std::string item;
            while (std::getline(cs, item, ';'))
            {
                size_t comma = item.find(',');
                if (comma != std::string::npos)
                {
                    try
                    {
                        int r = std::stoi(item.substr(0, comma));
                        int c = std::stoi(item.substr(comma + 1));
                        st.winCells.emplace(r, c);
                    }
                    catch (...)
                    {
                    }
                }
            }
        }
        if (!st.lastBoard.empty())
        {
            clearScreen();
            drawBoard(st);
        }
        bool iWon = (!side.empty() && side[0] == st.mySide);
        std::cout << GREEN << BOLD << "=== ПОБЕДА: " << side
                  << (st.mySide != '?' ? (iWon ? " (Вы победили!)" : " (Вы проиграли)") : "")
                  << " ===" << RESET << "\n";
        st.mySide = '?';
        st.currentTurn = '?';
        st.oppName.clear();
    }
    else if (tag == "DRAW")
    {
        std::cout << YELLOW << BOLD << "=== НИЧЬЯ ===" << RESET << "\n";
        st.mySide = '?';
        st.currentTurn = '?';
        st.oppName.clear();
    }
    else if (tag == "ERROR")
    {
        std::string e = ln.size() > 6 ? ln.substr(6) : "";
        if (e == "bad_credentials")
            std::cout << RED << "Неверный логин или пароль." << RESET << "\n";
        else if (e == "bad_username")
            std::cout << RED << "Недопустимое имя (разрешены A-Z, a-z, 0-9, _, -, до 32 символов)." << RESET << "\n";
        else if (e == "bad_password")
            std::cout << RED << "Недопустимый пароль (без пробелов, 1..128 символов)." << RESET << "\n";
        else if (e == "user_exists")
            std::cout << RED << "Имя уже занято." << RESET << "\n";
        else if (e == "already_logged_in")
            std::cout << RED << "Вы уже вошли в аккаунт." << RESET << "\n";
        else if (e == "already_in_game")
            std::cout << RED << "Вы уже в партии." << RESET << "\n";
        else if (e == "bad_params")
            std::cout << RED << "Неверные N/K: 3<=N<=10, 3<=K<=N." << RESET << "\n";
        else if (e == "cannot_play_self")
            std::cout << RED << "Нельзя играть с самим собой." << RESET << "\n";
        else if (e == "already_online")
            std::cout << RED << "Этот пользователь уже в сети." << RESET << "\n";
        else if (e == "not_logged_in")
            std::cout << RED << "Сначала войдите: LOGIN <user> [password]" << RESET << "\n";
        else if (e == "unknown_cmd")
            std::cout << RED << "Неизвестная команда. Введите HELP." << RESET << "\n";
        else if (e == "opponent_offline")
            std::cout << RED << "Игрок не в сети." << RESET << "\n";
        else if (e == "opponent_busy")
            std::cout << RED << "Игрок сейчас занят в другой партии." << RESET << "\n";
        else if (e == "no_invites")
            std::cout << YELLOW << "У вас нет активных приглашений." << RESET << "\n";
        else if (e == "multiple_invites_specify_user")
            std::cout << YELLOW << "Несколько приглашений — укажите имя: ACCEPT <user>" << RESET << "\n";
        else if (e == "NOT_YOUR_TURN")
            std::cout << RED << "Сейчас не ваш ход." << RESET << "\n";
        else if (e == "CELL_TAKEN")
            std::cout << RED << "Эта клетка уже занята." << RESET << "\n";
        else if (e == "BAD_CELL")
            std::cout << RED << "Координаты вне поля." << RESET << "\n";
        else if (e.rfind("usage: ", 0) == 0)
            std::cout << RED << "Формат: " << e.substr(7) << RESET << "\n";
        else
            std::cout << RED << ln << RESET << "\n";
    }
    else if (tag == "OK")
    {
        std::string what, who;
        ss >> what >> who;
        if (what == "LOGIN")
        {
            st.myName = who;
            st.mySide = '?';
            st.oppName.clear();
            std::cout << GREEN << BOLD << "Вход выполнен: " << who << RESET << "\n";
        }
        else if (what == "REGISTER")
        {
            std::cout << GREEN << BOLD << "Регистрация успешна: " << who
                      << ". Теперь войдите: LOGIN " << who << RESET << "\n";
        }
        else if (what == "CANCEL")
        {
            std::cout << YELLOW << "Приглашения и поиск игры отменены." << RESET << "\n";
        }
        else
        {
            std::cout << GREEN << ln << RESET << "\n";
        }
    }
    else if (tag == "QUEUED")
    {
        int N = 3, K = 3;
        ss >> N >> K;
        std::cout << CYAN << "Поиск соперника (" << N << "x" << N << ", K=" << K
                  << ")... Отмена: CANCEL" << RESET << "\n";
    }
    else if (tag == "GAME_START" || tag == "RESUMED")
    {
        std::string opp, side;
        int N = 3, K = 3;
        ss >> opp >> side >> N >> K;
        st.oppName = opp;
        st.mySide = side.empty() ? '?' : side[0];
        st.N = N;
        st.K = K;
        st.currentTurn = '?';
        st.lastR = st.lastC = -1;
        st.lastBoard.clear();
        st.winCells.clear();
        st.chatLog.clear();
        std::cout << CYAN << BOLD << "--- "
                  << (tag == "RESUMED" ? "Партия возобновлена: " : "Старт: ")
                  << "против " << opp << ", вы — " << side
                  << " (N=" << N << " K=" << K << ") ---" << RESET << "\n";
    }
    else if (tag == "RESUMED_AVAILABLE")
    {
        std::string opp, turn;
        ss >> opp >> turn;
        std::cout << YELLOW << "Есть сохранённая партия с " << BOLD << opp << RESET << YELLOW
                  << " (сейчас ход " << turn << "). Введите: RESUME" << RESET << "\n";
    }
    else if (tag == "WAIT_OPPONENT")
    {
        std::string opp;
        ss >> opp;
        std::cout << YELLOW << "Ожидание соперника " << opp << "..." << RESET << "\n";
    }
    else if (tag == "NO_SAVE")
    {
        std::cout << YELLOW << "Сохранённых партий нет." << RESET << "\n";
    }
    else if (tag == "OPPONENT_LEFT_SAVED")
    {
        std::cout << YELLOW << BOLD << "Соперник отключился, партия сохранена." << RESET << "\n";
        st.mySide = '?';
        st.currentTurn = '?';
        st.oppName.clear();
    }
    else if (tag == "WHO_BEGIN")
    {
        int cnt = 0;
        ss >> cnt;
        std::cout << CYAN << BOLD << "Игроки онлайн (" << cnt << "):" << RESET << "\n";
    }
    else if (tag == "WHO_USER")
    {
        std::string u, status;
        int elo = 1200;
        ss >> u >> elo >> status;
        std::cout << "  " << BOLD << u << RESET << " — Elo " << elo << " ["
                  << (status == "free" ? GREEN + std::string("свободен") : YELLOW + std::string("в игре"))
                  << RESET << "]\n";
    }
    else if (tag == "WHO_END")
    {
        // конец списка
    }
    else if (tag == "INVITE_SENT")
    {
        std::string opp;
        int N = 3, K = 3;
        ss >> opp >> N >> K;
        std::cout << CYAN << "Приглашение отправлено игроку " << opp
                  << " (" << N << "x" << N << ", K=" << K << ")." << RESET << "\n";
    }
    else if (tag == "INVITE_FROM")
    {
        std::string from;
        int N = 3, K = 3;
        ss >> from >> N >> K;
        std::cout << GREEN << BOLD << "Приглашение от " << from
                  << " (" << N << "x" << N << ", K=" << K << "). "
                  << "Принять: ACCEPT " << from << " | Отклонить: DECLINE " << from << RESET << "\n";
    }
    else if (tag == "INVITE_DECLINED")
    {
        std::string who;
        ss >> who;
        std::cout << YELLOW << who << " отклонил(а) ваше приглашение." << RESET << "\n";
    }
    else if (tag == "INVITE_CANCELED")
    {
        std::string who;
        ss >> who;
        std::cout << YELLOW << "Приглашение с " << who << " отменено." << RESET << "\n";
    }
    else if (tag == "OK_DECLINED")
    {
        std::string who;
        ss >> who;
        std::cout << YELLOW << "Вы отклонили приглашение от " << who << "." << RESET << "\n";
    }
    else if (tag == "OK_CANCELED")
    {
        std::string who;
        ss >> who;
        std::cout << YELLOW << "Приглашение для " << who << " отменено." << RESET << "\n";
    }
    else if (tag == "STATS_INFO")
    {
        std::string u;
        int elo = 0, w = 0, l = 0, d = 0;
        ss >> u >> elo >> w >> l >> d;
        int total = w + l + d;
        double wr = total > 0 ? (100.0 * w / total) : 0.0;
        std::cout << CYAN << BOLD << "Статистика " << u << ": " << RESET
                  << "Elo=" << BOLD << elo << RESET
                  << " | Игр=" << total
                  << " | " << GREEN << "W=" << w << RESET
                  << " / " << RED << "L=" << l << RESET
                  << " / " << YELLOW << "D=" << d << RESET
                  << " (" << std::fixed << std::setprecision(1) << wr << "% побед)\n";
    }
    else if (tag == "CHAT_MSG")
    {
        std::string scope, from;
        ss >> scope >> from;
        std::string rest;
        std::getline(ss, rest);
        rest = trim(rest);
        std::string formatted = std::string(scope == "game" ? MAGENTA "[Игра] " : CYAN "[Лобби] ") + BOLD + from + RESET + ": " + rest;
        if (scope == "game")
        {
            st.chatLog.push_back(formatted);
            while (st.chatLog.size() > 5)
                st.chatLog.pop_front();
        }
        std::cout << formatted << "\n";
    }
    else if (tag == "LEADERBOARD")
    {
        std::cout << CYAN << BOLD << ln << RESET << "\n";
    }
    else if (tag == "BYE")
    {
        std::cout << YELLOW << "До свидания!" << RESET << "\n";
        st.run = false;
    }
    else
    {
        std::cout << "[srv] " << ln << "\n";
    }

    drawPrompt(st);
}

static void processUserLine(ClientState &st, const std::string &rawLine)
{
    if (st.readingPassword)
    {
        std::string pw = trim(rawLine);
        std::string cmd = st.pwCmd;
        std::string user = st.pwUser;
        st.readingPassword = false;
        st.pwCmd.clear();
        st.pwUser.clear();
        st.pwBuf.clear();

        if (pw.empty())
        {
            clearPromptLine(st);
            std::cout << RED << "Пароль не введён — команда не отправлена." << RESET << "\n";
            drawPrompt(st);
            return;
        }
        if (pw.find_first_of(" \t") != std::string::npos)
        {
            clearPromptLine(st);
            std::cout << RED << "Пароль не должен содержать пробелы." << RESET << "\n";
            drawPrompt(st);
            return;
        }
        sendLine(st.sockFd, cmd + " " + user + " " + pw);
        drawPrompt(st);
        return;
    }

    std::string line = trim(rawLine);
    if (line.empty())
    {
        drawPrompt(st);
        return;
    }

    // Упрощённый ввод хода: "r c" во время активной партии
    if (st.mySide != '?')
    {
        int r = 0, c = 0;
        if (parseTwoInts(line, r, c))
        {
            sendLine(st.sockFd, "MOVE " + std::to_string(r) + " " + std::to_string(c));
            drawPrompt(st);
            return;
        }
    }

    std::string norm = upperFirstWord(line);
    std::istringstream ss(norm);
    std::string cmd;
    ss >> cmd;

    if (cmd == "HELP")
    {
        clearPromptLine(st);
        printHelp();
        drawPrompt(st);
        return;
    }
    if (cmd == "CLEAR")
    {
        clearScreen();
        if (!st.lastBoard.empty() && st.mySide != '?')
            drawBoard(st);
        drawPrompt(st);
        return;
    }
    if (cmd == "LOGIN" || cmd == "REGISTER")
    {
        std::string user, pw, extra;
        ss >> user >> pw;
        if (user.empty())
        {
            clearPromptLine(st);
            std::cout << RED << "Укажите имя: " << cmd << " <user> [password]" << RESET << "\n";
            drawPrompt(st);
            return;
        }
        if (ss >> extra)
        {
            clearPromptLine(st);
            std::cout << RED << "Пароль не должен содержать пробелы." << RESET << "\n";
            drawPrompt(st);
            return;
        }
        if (pw.empty())
        {
            st.readingPassword = true;
            st.pwCmd = cmd;
            st.pwUser = user;
            st.pwBuf.clear();
            drawPrompt(st);
            return;
        }
        sendLine(st.sockFd, cmd + " " + user + " " + pw);
        drawPrompt(st);
        return;
    }

    sendLine(st.sockFd, norm);
    if (cmd == "QUIT")
        st.quitSent = true;
    drawPrompt(st);
}

static void popUtf8Char(std::string &s)
{
    while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80)
    {
        s.pop_back();
    }
    if (!s.empty())
        s.pop_back();
}

int main(int argc, char **argv)
{
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    std::signal(SIGPIPE, SIG_IGN);

    std::string host = argc >= 2 ? argv[1] : "127.0.0.1";
    int port = 5555;
    if (argc >= 3)
    {
        try
        {
            port = std::stoi(argv[2]);
        }
        catch (...)
        {
            port = 5555;
        }
    }

    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        perror("socket");
        return 1;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1)
    {
        std::cerr << "bad host\n";
        ::close(fd);
        return 1;
    }
    if (::connect(fd, (sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("connect");
        ::close(fd);
        return 1;
    }

    TerminalGuard term;
    ClientState st;
    st.sockFd = fd;
    st.isTty = term.isTty;

    std::cout << RESET << "Connected to " << host << ':' << port << "\n";
    std::cout << "Введите HELP для списка команд.\n";
    drawPrompt(st);

    std::string netAcc;
    std::string stdinAcc;
    bool stdinEof = false;
    int eofDrainTicks = 0;

    while (st.run && !g_stop)
    {
        std::vector<pollfd> pfds;
        pfds.push_back(pollfd{fd, POLLIN, 0});
        if (!stdinEof)
        {
            pfds.push_back(pollfd{STDIN_FILENO, POLLIN, 0});
        }

        int ret = ::poll(pfds.data(), static_cast<nfds_t>(pfds.size()), 100);
        if (ret < 0)
        {
            if (errno == EINTR)
                continue;
            break;
        }
        if (ret == 0)
        {
            if (stdinEof && !st.quitSent)
            {
                if (++eofDrainTicks >= 5)
                    break;
            }
            continue;
        }

        if (pfds[0].revents & (POLLERR | POLLNVAL))
        {
            clearPromptLine(st);
            std::cout << RED << "\nОшибка соединения с сервером." << RESET << "\n";
            break;
        }
        if (pfds[0].revents & (POLLIN | POLLHUP))
        {
            char buf[4096];
            ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
            if (n <= 0)
            {
                clearPromptLine(st);
                if (!st.quitSent)
                {
                    std::cout << RED << "\nСоединение закрыто сервером." << RESET << "\n";
                }
                break;
            }
            netAcc.append(buf, static_cast<size_t>(n));
            size_t pos;
            while (st.run && (pos = netAcc.find('\n')) != std::string::npos)
            {
                std::string ln = netAcc.substr(0, pos);
                netAcc.erase(0, pos + 1);
                if (!ln.empty() && ln.back() == '\r')
                    ln.pop_back();
                handleServerLine(st, ln);
            }
            if (netAcc.size() > (1u << 20))
                netAcc.clear();
        }

        if (!stdinEof && pfds.size() > 1 && (pfds[1].revents & (POLLIN | POLLHUP)))
        {
            char buf[1024];
            ssize_t n = ::read(STDIN_FILENO, buf, sizeof(buf));
            if (n <= 0)
            {
                stdinEof = true;
                if (!st.isTty && !stdinAcc.empty())
                {
                    processUserLine(st, stdinAcc);
                    stdinAcc.clear();
                }
                continue;
            }

            if (st.isTty)
            {
                for (ssize_t i = 0; i < n && st.run; ++i)
                {
                    unsigned char ch = static_cast<unsigned char>(buf[i]);
                    if (st.escState == 1)
                    {
                        st.escState = (ch == '[' || ch == 'O') ? 2 : 0;
                        continue;
                    }
                    if (st.escState == 2)
                    {
                        if (ch >= 0x40 && ch <= 0x7E)
                            st.escState = 0;
                        continue;
                    }
                    if (ch == 27)
                    {
                        st.escState = 1;
                        continue;
                    }

                    if (ch == '\r' || ch == '\n')
                    {
                        std::cout << "\n";
                        if (st.readingPassword)
                        {
                            std::string pw = st.pwBuf;
                            processUserLine(st, pw);
                        }
                        else
                        {
                            std::string line = st.inputBuf;
                            st.inputBuf.clear();
                            processUserLine(st, line);
                        }
                    }
                    else if (ch == 0x7f || ch == 0x08)
                    {
                        if (st.readingPassword)
                            popUtf8Char(st.pwBuf);
                        else
                            popUtf8Char(st.inputBuf);
                        drawPrompt(st);
                    }
                    else if (ch == 0x04)
                    {
                        if (!st.readingPassword && st.inputBuf.empty())
                        {
                            std::cout << "\n";
                            sendLine(fd, "QUIT");
                            st.quitSent = true;
                        }
                    }
                    else if (ch == 0x03)
                    {
                        st.run = false;
                        break;
                    }
                    else if (ch >= 32)
                    {
                        if (st.readingPassword)
                        {
                            if (st.pwBuf.size() < 128)
                                st.pwBuf.push_back(static_cast<char>(ch));
                        }
                        else
                        {
                            if (st.inputBuf.size() < 512)
                                st.inputBuf.push_back(static_cast<char>(ch));
                        }
                        drawPrompt(st);
                    }
                }
            }
            else
            {
                stdinAcc.append(buf, static_cast<size_t>(n));
                size_t pos;
                while (st.run && (pos = stdinAcc.find('\n')) != std::string::npos)
                {
                    std::string ln = stdinAcc.substr(0, pos);
                    stdinAcc.erase(0, pos + 1);
                    if (!ln.empty() && ln.back() == '\r')
                        ln.pop_back();
                    processUserLine(st, ln);
                }
            }
        }
    }

    clearPromptLine(st);
    ::shutdown(fd, SHUT_RDWR);
    ::close(fd);
    return 0;
}