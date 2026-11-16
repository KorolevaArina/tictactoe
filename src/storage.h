#pragma once
#include <map>
#include <string>


bool valid_username(const std::string& u);  // [A-Z a-z 0-9 _-], 1..32
bool valid_password(const std::string& p);  // �������� ASCII ��� ��������, 1..128


void log_event(const std::string& msg);


struct UserStats {
    int wins = 0, losses = 0, draws = 0;
};


class Storage {
public:
    explicit Storage(std::string usersFile = "users.txt");
    bool registerUser(const std::string& user, const std::string& pass);
    bool checkLogin(const std::string& user, const std::string& pass) const;
    bool getStats(const std::string& user, UserStats& out) const;
    void recordResult(const std::string& a, const std::string& b, double scoreA);
private:
    struct Account {
        std::string pass;  // TODO: ���������� ������
        UserStats stats;
    };
    void loadUsers();
    void writeUsers() const;

    std::string usersFile_;
    std::map<std::string, Account> accounts_;
};