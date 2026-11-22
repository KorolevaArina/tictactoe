// protocol.cpp — обработка команд клиента.
//
// Клиент -> сервер:
//   REGISTER user pass | LOGIN user pass | TOP | STATS [user] | WHO
//   NEWGAME [N K] [opponent]   — без соперника: очередь; с соперником: приглашение
//   ACCEPT [user] | DECLINE [user] | CANCEL
//   MOVE r c | CHAT text | RESUME | QUIT
// Сервер -> клиент (основное):
//   OK ... | ERROR <code> | BOARD X O _ ... | TURN X | LAST r c | WIN X [r,c;r,c...] | DRAW
//   LEADERBOARD 1.alice 1350 ... | RESUMED_AVAILABLE opp turn | NO_SAVE | WAIT_OPPONENT opp
//   GAME_START opp side N K | RESUMED opp side N K | OPPONENT_LEFT_SAVED | QUEUED N K
//   INVITE_SENT / INVITE_FROM / INVITE_DECLINED / INVITE_CANCELED | PING | BYE
#include "protocol.h"

#include <cctype>
#include <sstream>

namespace
{

    std::vector<std::string> split(const std::string &line)
    {
        std::vector<std::string> out;
        std::istringstream ss(line);
        std::string w;
        while (ss >> w)
            out.push_back(w);
        return out;
    }

    std::string upper(std::string s)
    {
        for (char &ch : s)
            ch = char(std::toupper(static_cast<unsigned char>(ch)));
        return s;
    }

    // Строгий разбор целого числа (без мусора в конце, в разумных пределах).
    bool parseInt(const std::string &s, int &out)
    {
        if (s.empty() || s.size() > 6)
            return false;
        size_t i = (s[0] == '-' || s[0] == '+') ? 1 : 0;
        if (i == s.size())
            return false;
        for (size_t j = i; j < s.size(); ++j)
            if (!std::isdigit(static_cast<unsigned char>(s[j])))
                return false;
        out = std::stoi(s);
        return true;
    }

    // Сообщение чата: без управляющих символов, не длиннее 256 байт.
    std::string sanitizeChat(const std::string &raw)
    {
        std::string out;
        for (unsigned char ch : raw)
        {
            if (ch < 32 || ch == 127)
                continue;
            if (out.size() >= 256)
                break;
            out.push_back(char(ch));
        }
        // Не оставляем «обрезанный» UTF-8 символ в конце.
        size_t cut = out.size();
        while (cut > 0 && (static_cast<unsigned char>(out[cut - 1]) & 0xC0) == 0x80)
            --cut;
        if (cut > 0 && (static_cast<unsigned char>(out[cut - 1]) & 0x80))
        {
            unsigned char lead = static_cast<unsigned char>(out[cut - 1]);
            size_t need = (lead >= 0xF0) ? 4 : (lead >= 0xE0) ? 3
                                                              : 2;
            if (out.size() - (cut - 1) < need)
                out.resize(cut - 1);
        }
        size_t b = out.find_first_not_of(' ');
        return b == std::string::npos ? "" : out.substr(b, out.find_last_not_of(' ') - b + 1);
    }

    std::string sideStr(char c) { return std::string(1, c); }

} // namespace

// ======================= Диспетчер =======================

void Protocol::onLine(int fd, const std::string &line)
{
    Args a = split(line);
    if (a.empty())
        return;
    const std::string cmd = upper(a[0]);

    // Команды, доступные без входа.
    if (cmd == "REGISTER")
        return cmdRegister(fd, a);
    if (cmd == "LOGIN")
        return cmdLogin(fd, a);
    if (cmd == "TOP")
        return cmdTop(fd);
    if (cmd == "QUIT")
    {
        net_.send(fd, "BYE");
        net_.closeAfterFlush(fd); // onDisconnect вызовется при закрытии
        return;
    }

    auto it = userByFd_.find(fd);
    const bool known = cmd == "STATS" || cmd == "WHO" || cmd == "NEWGAME" || cmd == "ACCEPT" ||
                       cmd == "DECLINE" || cmd == "CANCEL" || cmd == "MOVE" || cmd == "CHAT" ||
                       cmd == "RESUME";
    if (!known)
        return net_.send(fd, "ERROR unknown_cmd");
    if (it == userByFd_.end())
        return net_.send(fd, "ERROR not_logged_in");
    const std::string me = it->second; // копия: обработчики могут менять карты

    if (cmd == "STATS")
        cmdStats(fd, me, a);
    else if (cmd == "WHO")
        cmdWho(fd);
    else if (cmd == "NEWGAME")
        cmdNewGame(fd, me, a);
    else if (cmd == "ACCEPT")
        cmdAccept(fd, me, a, true);
    else if (cmd == "DECLINE")
        cmdAccept(fd, me, a, false);
    else if (cmd == "CANCEL")
        cmdCancel(fd, me);
    else if (cmd == "MOVE")
        cmdMove(fd, me, a);
    else if (cmd == "CHAT")
        cmdChat(fd, me, line);
    else if (cmd == "RESUME")
        cmdResume(fd, me);
}

// ======================= Аккаунты =======================

void Protocol::cmdRegister(int fd, const Args &a)
{
    if (a.size() != 3)
        return net_.send(fd, "ERROR usage: REGISTER user pass");
    if (!valid_username(a[1]))
        return net_.send(fd, "ERROR bad_username");
    if (!valid_password(a[2]))
        return net_.send(fd, "ERROR bad_password");
    if (!storage_.registerUser(a[1], a[2]))
        return net_.send(fd, "ERROR user_exists");
    log_event("REGISTER " + a[1]);
    net_.send(fd, "OK REGISTER " + a[1]);
}

void Protocol::cmdLogin(int fd, const Args &a)
{
    if (userByFd_.count(fd))
        return net_.send(fd, "ERROR already_logged_in");
    if (a.size() != 3)
        return net_.send(fd, "ERROR usage: LOGIN user pass");
    const std::string &u = a[1];
    if (!storage_.checkLogin(u, a[2]))
        return net_.send(fd, "ERROR bad_credentials");
    if (online(u))
        return net_.send(fd, "ERROR already_online");

    userByFd_[fd] = u;
    fdByUser_[u] = fd;
    log_event("LOGIN " + u);
    net_.send(fd, "OK LOGIN " + u);

    // Соперник уже ждёт этого игрока после RESUME — сразу продолжаем партию.
    for (const auto &[waiter, sg] : pendingResume_)
    {
        if ((sg.p1 == u || sg.p2 == u) && waiter != u && online(waiter) && !inGame(waiter))
        {
            SavedGame copy = sg; // startResumed изменяет pendingResume_
            startResumed(copy);
            return;
        }
    }
    SavedGame sg;
    if (storage_.findSave(u, sg))
        net_.send(fd, "RESUMED_AVAILABLE " + (sg.p1 == u ? sg.p2 : sg.p1) + " " + sideStr(sg.turn));
}

void Protocol::cmdTop(int fd)
{
    std::string out = "LEADERBOARD";
    int place = 1;
    for (const auto &e : storage_.leaderboard(5))
        out += " " + std::to_string(place++) + "." + e.name + " " + std::to_string(e.stats.elo);
    net_.send(fd, out);
}

void Protocol::cmdStats(int fd, const std::string &me, const Args &a)
{
    if (a.size() > 2)
        return net_.send(fd, "ERROR usage: STATS [user]");
    const std::string target = a.size() == 2 ? a[1] : me;
    UserStats s;
    if (!storage_.getStats(target, s))
        return net_.send(fd, "ERROR user_not_found");
    net_.send(fd, "STATS_INFO " + target + " " + std::to_string(s.elo) + " " +
                      std::to_string(s.wins) + " " + std::to_string(s.losses) + " " +
                      std::to_string(s.draws));
}

void Protocol::cmdWho(int fd)
{
    net_.send(fd, "WHO_BEGIN " + std::to_string(fdByUser_.size()));
    for (const auto &[u, ufd] : fdByUser_)
    {
        UserStats s;
        storage_.getStats(u, s);
        net_.send(fd, "WHO_USER " + u + " " + std::to_string(s.elo) + " " +
                          (inGame(u) ? "in_game" : "free"));
    }
    net_.send(fd, "WHO_END");
}

// ======================= Лобби =======================

void Protocol::sendTo(const std::string &user, const std::string &line)
{
    auto it = fdByUser_.find(user);
    if (it != fdByUser_.end())
        net_.send(it->second, line);
}

void Protocol::clearLobbyState(const std::string &user, const std::string &except)
{
    for (auto it = invites_.begin(); it != invites_.end();)
    {
        const auto &[from, to] = it->first;
        if (from == user || to == user)
        {
            const std::string other = (from == user) ? to : from;
            if (other != except)
                sendTo(other, "INVITE_CANCELED " + user);
            it = invites_.erase(it);
        }
        else
        {
            ++it;
        }
    }
    for (auto it = queue_.begin(); it != queue_.end();)
        it = (it->second == user) ? queue_.erase(it) : std::next(it);
    pendingResume_.erase(user);
}

void Protocol::cmdNewGame(int fd, const std::string &me, const Args &a)
{
    if (inGame(me))
        return net_.send(fd, "ERROR already_in_game");

    int n = 3, k = 3;
    std::string opp;
    const size_t cnt = a.size() - 1;
    if (cnt == 2 || cnt == 3)
    {
        if (!parseInt(a[1], n) || !parseInt(a[2], k))
            return net_.send(fd, "ERROR usage: NEWGAME [N K] [opponent]");
        if (cnt == 3)
            opp = a[3];
    }
    else if (cnt == 1)
    {
        opp = a[1];
    }
    else if (cnt != 0)
    {
        return net_.send(fd, "ERROR usage: NEWGAME [N K] [opponent]");
    }
    if (!Game::validParams(n, k))
        return net_.send(fd, "ERROR bad_params");
    const std::string nk = std::to_string(n) + " " + std::to_string(k);

    if (!opp.empty())
    { // приглашение конкретному игроку
        if (opp == me)
            return net_.send(fd, "ERROR cannot_play_self");
        if (!online(opp))
            return net_.send(fd, "ERROR opponent_offline");
        if (inGame(opp))
            return net_.send(fd, "ERROR opponent_busy");
        invites_[{me, opp}] = Invite{n, k};
        net_.send(fd, "INVITE_SENT " + opp + " " + nk);
        sendTo(opp, "INVITE_FROM " + me + " " + nk);
        return;
    }

    // Очередь: ждём любого игрока с теми же N и K.
    for (auto it = queue_.begin(); it != queue_.end();)
        it = (it->second == me) ? queue_.erase(it) : std::next(it);
    auto q = queue_.find({n, k});
    if (q != queue_.end())
    {
        const std::string first = q->second;
        queue_.erase(q);
        startGame(first, me, n, k); // кто ждал дольше, тот ходит первым
    }
    else
    {
        queue_[{n, k}] = me;
        net_.send(fd, "QUEUED " + nk);
    }
}

void Protocol::cmdAccept(int fd, const std::string &me, const Args &a, bool accept)
{
    if (a.size() > 2)
        return net_.send(fd, accept ? "ERROR usage: ACCEPT [user]" : "ERROR usage: DECLINE [user]");
    if (accept && inGame(me))
        return net_.send(fd, "ERROR already_in_game");

    std::string from;
    if (a.size() == 2)
    {
        from = a[1];
        if (!invites_.count({from, me}))
            return net_.send(fd, "ERROR no_such_invite");
    }
    else
    {
        int cnt = 0;
        for (const auto &[key, inv] : invites_)
            if (key.second == me)
            {
                from = key.first;
                ++cnt;
            }
        if (cnt == 0)
            return net_.send(fd, "ERROR no_invites");
        if (cnt > 1)
            return net_.send(fd, "ERROR multiple_invites_specify_user");
    }

    const Invite inv = invites_[{from, me}];
    invites_.erase({from, me});
    if (!accept)
    {
        net_.send(fd, "OK_DECLINED " + from);
        sendTo(from, "INVITE_DECLINED " + me);
        return;
    }
    if (!online(from))
        return net_.send(fd, "ERROR opponent_offline");
    if (inGame(from))
        return net_.send(fd, "ERROR opponent_busy");
    startGame(from, me, inv.n, inv.k);
}

void Protocol::cmdCancel(int fd, const std::string &me)
{
    clearLobbyState(me);
    net_.send(fd, "OK CANCEL");
}

// ======================= Партия =======================

void Protocol::startGame(const std::string &x, const std::string &o, int n, int k)
{
    clearLobbyState(x, o);
    clearLobbyState(o, x);
    // Новая игра: старая сохранённая партия каждого из игроков удаляется
    // (перезаписывается). Поэтому у игрока всегда не больше одной сохранёнки.
    storage_.eraseSavesWith(x);
    storage_.eraseSavesWith(o);

    auto m = std::make_shared<Match>();
    m->x = x;
    m->o = o;
    m->game = Game(n, k);
    matches_[x] = m;
    matches_[o] = m;
    log_event("GAME_START " + x + " " + o + " " + std::to_string(n) + "x" + std::to_string(n) +
              " K=" + std::to_string(k));

    const std::string nk = " " + std::to_string(n) + " " + std::to_string(k);
    sendTo(x, "GAME_START " + o + " X" + nk);
    sendTo(o, "GAME_START " + x + " O" + nk);
    sendBoard(*m);
}

void Protocol::startResumed(const SavedGame &sg)
{
    Game g;
    if (!Game::restore(sg.n, sg.k, sg.board, sg.turn, g))
    {
        storage_.eraseSavesWith(sg.p1); // повреждённое сохранение
        return;
    }
    clearLobbyState(sg.p1, sg.p2);
    clearLobbyState(sg.p2, sg.p1);
    // Партия снова в памяти; если кто-то опять отключится — сохраним заново.
    storage_.eraseSavesWith(sg.p1);
    storage_.eraseSavesWith(sg.p2);
    log_event("LOAD_SAVE " + sg.p1 + " " + sg.p2);

    auto m = std::make_shared<Match>();
    m->x = sg.p1;
    m->o = sg.p2;
    m->game = g;
    matches_[m->x] = m;
    matches_[m->o] = m;

    const std::string nk = " " + std::to_string(sg.n) + " " + std::to_string(sg.k);
    sendTo(m->x, "RESUMED " + m->o + " X" + nk);
    sendTo(m->o, "RESUMED " + m->x + " O" + nk);
    sendBoard(*m);
}

void Protocol::sendBoard(const Match &m)
{
    const Game &g = m.game;
    for (const std::string &who : {m.x, m.o})
    {
        if (g.lastRow() >= 0)
            sendTo(who, "LAST " + std::to_string(g.lastRow()) + " " + std::to_string(g.lastCol()));
        if (!g.finished())
            sendTo(who, "TURN " + sideStr(g.turn()));
        sendTo(who, g.boardLine());
    }
}

void Protocol::cmdMove(int fd, const std::string &me, const Args &a)
{
    if (a.size() != 3)
        return net_.send(fd, "ERROR usage: MOVE r c");
    auto it = matches_.find(me);
    if (it == matches_.end())
        return net_.send(fd, "ERROR no_active_game");
    int r, c;
    if (!parseInt(a[1], r) || !parseInt(a[2], c))
        return net_.send(fd, "ERROR usage: MOVE r c");

    std::shared_ptr<Match> m = it->second; // копия указателя: finishMatch удалит запись
    const char sym = (me == m->x) ? 'X' : 'O';
    MoveResult res = m->game.move(sym, r, c);
    if (res != MoveResult::Ok)
        return net_.send(fd, std::string("ERROR ") + moveErrorCode(res));

    log_event("MOVE " + me + " " + std::to_string(r) + " " + std::to_string(c));
    sendBoard(*m);
    if (m->game.finished())
        finishMatch(m);
}

void Protocol::finishMatch(std::shared_ptr<Match> m)
{
    const Game &g = m->game;
    if (g.isDraw())
    {
        storage_.recordResult(m->x, m->o, 0.5);
        log_event("GAME_END DRAW " + m->x + " " + m->o);
        sendTo(m->x, "DRAW");
        sendTo(m->o, "DRAW");
    }
    else
    {
        const bool xWon = g.winner() == 'X';
        storage_.recordResult(m->x, m->o, xWon ? 1.0 : 0.0);
        log_event("GAME_END WIN " + (xWon ? m->x : m->o) + " LOSS " + (xWon ? m->o : m->x));
        std::string line = "WIN " + sideStr(g.winner()) + " ";
        for (size_t i = 0; i < g.winLine().size(); ++i)
        {
            if (i)
                line += ';';
            line += std::to_string(g.winLine()[i].first) + "," + std::to_string(g.winLine()[i].second);
        }
        sendTo(m->x, line);
        sendTo(m->o, line);
    }
    matches_.erase(m->x);
    matches_.erase(m->o);
}

void Protocol::cmdChat(int fd, const std::string &me, const std::string &line)
{
    size_t sp = line.find_first_of(" \t");
    std::string msg = sanitizeChat(sp == std::string::npos ? "" : line.substr(sp + 1));
    if (msg.empty())
        return net_.send(fd, "ERROR empty_message");

    auto it = matches_.find(me);
    if (it != matches_.end())
    { // в партии — только сопернику
        std::string pkt = "CHAT_MSG game " + me + " " + msg;
        sendTo(it->second->x, pkt);
        sendTo(it->second->o, pkt);
    }
    else
    { // в лобби — всем, кто не в игре
        std::string pkt = "CHAT_MSG lobby " + me + " " + msg;
        for (const auto &[u, ufd] : fdByUser_)
            if (!inGame(u))
                net_.send(ufd, pkt);
    }
}

// ======================= Сохранение / возобновление =======================

void Protocol::cmdResume(int fd, const std::string &me)
{
    if (inGame(me))
        return net_.send(fd, "ERROR already_in_game");
    SavedGame sg;
    if (!storage_.findSave(me, sg))
        return net_.send(fd, "NO_SAVE");

    log_event("RESUME " + me);
    const std::string opp = (sg.p1 == me) ? sg.p2 : sg.p1;
    if (inGame(opp))
        return net_.send(fd, "ERROR opponent_busy");
    if (online(opp))
    {
        startResumed(sg); // второй игрок онлайн — продолжаем сразу
    }
    else
    {
        // Второго нет: держим партию в памяти, пока он не войдёт (см. cmdLogin).
        clearLobbyState(me);
        pendingResume_[me] = sg;
        net_.send(fd, "WAIT_OPPONENT " + opp);
    }
}

void Protocol::onDisconnect(int fd)
{
    auto it = userByFd_.find(fd);
    if (it == userByFd_.end())
        return;
    const std::string u = it->second;

    auto mit = matches_.find(u);
    if (mit != matches_.end())
    {
        std::shared_ptr<Match> m = mit->second;
        if (!m->game.finished())
        {
            const Game &g = m->game;
            storage_.saveGame(SavedGame{m->x, m->o, g.turn(), g.size(), g.winLength(), g.cells()});
            log_event("SAVE " + m->x + " " + m->o);
            sendTo(u == m->x ? m->o : m->x, "OPPONENT_LEFT_SAVED");
        }
        matches_.erase(m->x);
        matches_.erase(m->o);
    }
    clearLobbyState(u);
    userByFd_.erase(fd);
    fdByUser_.erase(u);
    log_event("DISCONNECT " + u);
}
