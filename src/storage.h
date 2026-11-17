#pragma once

#include <map>
#include <string>
#include <vector>

bool valid_username(const std::string& u);
bool valid_password(const std::string& p);

void log_event(const std::string& msg);


struct UserStats {
    int wins = 0, losses = 0, draws = 0;
};

// Строка saves.txt: "player1 player2 turn N K board"
struct SavedGame {
    std::string p1, p2;
    char turn = 'X';
    int n = 3, k = 3;
    std::string board;
};

class Storage {
public:
    explicit Storage(std::string usersFile = "users.txt", std::string savesFile = "saves.txt");

    bool registerUser(const std::string& user, const std::string& pass);
    bool checkLogin(const std::string& user, const std::string& pass) const;
    bool getStats(const std::string& user, UserStats& out) const;
    void recordResult(const std::string& a, const std::string& b, double scoreA);

    void saveGame(const SavedGame& g);
    bool findSave(const std::string& user, SavedGame& out) const;
    void eraseSavesWith(const std::string& user);

private:
    struct Account {
        std::string pass;  // TODO: хэшировать пароли
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