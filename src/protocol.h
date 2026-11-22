// protocol.h — разбор текстовых команд клиента и логика лобби:
// вход, приглашения/очередь, активные партии, сохранение/возобновление.
#pragma once

#include "game.h"
#include "network.h"
#include "storage.h"

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class Protocol : public NetHandler
{
public:
    Protocol(Network &net, Storage &storage) : net_(net), storage_(storage) {}

    void onLine(int fd, const std::string &line) override;
    void onDisconnect(int fd) override;

private:
    struct Match
    {
        std::string x, o; // x ходит крестиками и всегда первый игрок в saves.txt
        Game game;
    };
    struct Invite
    {
        int n = 3, k = 3;
    };
    using Args = std::vector<std::string>;

    // --- команды ---
    void cmdRegister(int fd, const Args &a);
    void cmdLogin(int fd, const Args &a);
    void cmdTop(int fd);
    void cmdStats(int fd, const std::string &me, const Args &a);
    void cmdWho(int fd);
    void cmdNewGame(int fd, const std::string &me, const Args &a);
    void cmdAccept(int fd, const std::string &me, const Args &a, bool accept);
    void cmdCancel(int fd, const std::string &me);
    void cmdMove(int fd, const std::string &me, const Args &a);
    void cmdChat(int fd, const std::string &me, const std::string &line);
    void cmdResume(int fd, const std::string &me);

    // --- вспомогательное ---
    void sendTo(const std::string &user, const std::string &line);
    bool online(const std::string &user) const { return fdByUser_.count(user) > 0; }
    bool inGame(const std::string &user) const { return matches_.count(user) > 0; }
    // Убрать приглашения, место в очереди и ожидание RESUME пользователя.
    void clearLobbyState(const std::string &user, const std::string &except = "");
    void startGame(const std::string &x, const std::string &o, int n, int k);
    void startResumed(const SavedGame &sg);
    void sendBoard(const Match &m);
    void finishMatch(std::shared_ptr<Match> m); // по значению: запись в matches_ удаляется

    Network &net_;
    Storage &storage_;

    std::map<int, std::string> userByFd_;
    std::map<std::string, int> fdByUser_;
    std::map<std::string, std::shared_ptr<Match>> matches_;         // игрок -> партия
    std::map<std::pair<std::string, std::string>, Invite> invites_; // (от кого, кому)
    std::map<std::pair<int, int>, std::string> queue_;              // (N, K) -> ждущий игрок
    std::map<std::string, SavedGame> pendingResume_;                // кто ждёт -> партия
};
