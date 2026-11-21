#ifndef STORAGE_H
#define STORAGE_H

#include <cstddef>
#include <map>
#include <string>
#include <vector>

static constexpr int kInitialElo = 1200;
static constexpr int kEloK = 32;

struct UserStats {
    int wins = 0;
    int losses = 0;
    int draws = 0;
    int elo = kInitialElo;
};

struct Account {
    std::string pass;   // TODO: хэшировать вместо открытого текста
    UserStats stats;
};

struct SavedGame {
    std::string p1;     // всегда играет за 'X'
    std::string p2;     // всегда играет за 'O'
    char turn = 'X';
    int n = 3;
    int k = 3;
    std::string board;  // n*n символов 'X'/'O'/'_'
};

struct LeaderEntry {
    std::string name;
    UserStats stats;
};

bool valid_username(const std::string& u);
bool valid_password(const std::string& p);
void log_event(const std::string& msg);

double elo_expected(int player, int opponent);
int elo_updated(int player, int opponent, double score);

class Storage {
public:
    Storage(std::string usersFile, std::string savesFile);

    bool registerUser(const std::string& user, const std::string& pass);
    bool checkLogin(const std::string& user, const std::string& pass) const;
    bool getStats(const std::string& user, UserStats& out) const;
    void recordResult(const std::string& a, const std::string& b, double scoreA);
    std::vector<LeaderEntry> leaderboard(size_t limit) const;

    void saveGame(const SavedGame& g);
    bool findSave(const std::string& user, SavedGame& out) const;
    void eraseSavesWith(const std::string& user);

private:
    void loadUsers();
    void writeUsers() const;
    void loadSaves();
    void writeSaves() const;

    std::string usersFile_;
    std::string savesFile_;
    std::map<std::string, Account> accounts_;
    std::vector<SavedGame> saves_;
};

#endif