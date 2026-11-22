#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

// ---------- Валидация ----------
bool valid_username(const std::string& u); // [A-Za-z0-9_-], 1..32
bool valid_password(
    const std::string& p); // печатные ASCII без пробелов, 1..128

// ---------- Elo ----------
constexpr int kInitialElo = 1200;
constexpr double kEloK = 32.0;
// expected = 1 / (1 + 10^((opponent - player) / 400))
double elo_expected(int player, int opponent);
// new = old + K * (score - expected); score: 1 победа, 0.5 ничья, 0 поражение.
int elo_updated(int player, int opponent, double score);

// ---------- Лог ----------
// Пишет "[YYYY-MM-DD HH:MM:SS] msg" в server.log (режим дозаписи).
void log_event(const std::string& msg);
void set_log_path(const std::string& path); // для тестов

// ---------- Данные ----------
struct UserStats {
    int wins = 0, losses = 0, draws = 0;
    int elo = kInitialElo;
};

struct LeaderEntry {
    std::string name;
    UserStats stats;
};

// Строка saves.txt: "player1 player2 turn N K board"
// player1 всегда играет X, player2 — O; board — N*N символов X/O/_.
struct SavedGame {
    std::string p1, p2;
    char turn = 'X';
    int n = 3, k = 3;
    std::string board;
};

class Storage {
public:
    explicit Storage(std::string usersFile = "users.txt",
        std::string savesFile = "saves.txt");

    // ---- Аккаунты ----
    bool registerUser(const std::string& user, const std::string& pass);
    bool checkLogin(const std::string& user, const std::string& pass) const;
    bool getStats(const std::string& user, UserStats& out) const;

    // Итог партии: scoreA = 1 (победил a), 0.5 (ничья), 0 (победил b).
    // Обновляет wins/losses/draws и Elo обоих игроков и сразу пишет users.txt.
    void recordResult(const std::string& a, const std::string& b, double scoreA);

    // Топ по Elo (по убыванию; при равенстве — по победам, затем по имени).
    std::vector<LeaderEntry> leaderboard(size_t limit = 5) const;

    // ---- Сохранённые партии ----
    // У каждого игрока не более одной сохранённой партии: при сохранении
    // старые партии обоих участников заменяются новой.
    void saveGame(const SavedGame& g);
    bool findSave(const std::string& user, SavedGame& out) const;
    void eraseSavesWith(const std::string& user);

private:
    struct Account {
        std::string passHash;
        UserStats stats;
    };

    void loadUsers();
    void loadSaves();
    void writeUsers() const;
    void writeSaves() const;

    std::string usersFile_, savesFile_;
    std::map<std::string, Account> accounts_;
    std::vector<SavedGame> saves_;
};