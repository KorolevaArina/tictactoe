#include "storage.h"

#include <cctype>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>


bool valid_username(const std::string& u) {
    if (u.empty() || u.size() > 32) {
        return false;
    }
    for (unsigned char ch : u)
        if (!std::isalnum(ch) && ch != '_' && ch != '-') {
            return false;
        }
    return true;
}


bool valid_password(const std::string& p) {
    if (p.empty() || p.size() > 128) {
        return false;
    }
    for (unsigned char ch : p) {
        if (ch < 33 || ch > 126) {
            return false;
        }
    }
    return true;
}


void log_event(const std::string& msg) {
    std::ofstream out("server.log", std::ios::app);
    if (!out) {
        return;
    }
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    out << '[' << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << "] " << msg << '\n';
}


static void atomicWrite(const std::string& path, const std::string& content) {
    std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        out << content;
        if (!out) {
            std::cerr << "storage: cannot write " << tmp << "\n";
            return;
        }
    }
    if (std::rename(tmp.c_str(), path.c_str()) != 0)
        std::cerr << "storage: cannot rename " << tmp << "\n";
}


Storage::Storage(std::string usersFile) : usersFile_(std::move(usersFile)) {
    loadUsers();
}


void Storage::loadUsers() {
    std::ifstream in(usersFile_);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        std::string user;
        Account a;
        if (!(ss >> user >> a.pass >> a.stats.wins >> a.stats.losses >> a.stats.draws)) {
            continue;
        }
        if (valid_username(user) && valid_password(a.pass)) {
            accounts_[user] = a;
        }
    }
}


void Storage::writeUsers() const {
    std::ostringstream out;
    for (const auto& [user, a] : accounts_) {
        out << user << ' ' << a.pass << ' ' << a.stats.wins << ' ' << a.stats.losses << ' '
            << a.stats.draws << '\n';
    }
    atomicWrite(usersFile_, out.str());
}


bool Storage::registerUser(const std::string& user, const std::string& pass) {
    if (!valid_username(user) || !valid_password(pass) || accounts_.count(user)) {
        return false;
    }
    Account a;
    a.pass = pass;  // TODO: хэшировать вместо открытого текста
    accounts_[user] = a;
    writeUsers();
    return true;
}


bool Storage::checkLogin(const std::string& user, const std::string& pass) const {
    auto it = accounts_.find(user);
    return it != accounts_.end() && it->second.pass == pass;
}


bool Storage::getStats(const std::string& user, UserStats& out) const {
    auto it = accounts_.find(user);
    if (it == accounts_.end()) {
        return false;
    }
    out = it->second.stats;
    return true;
}


void Storage::recordResult(const std::string& a, const std::string& b, double scoreA) {
    auto ia = accounts_.find(a);
    auto ib = accounts_.find(b);
    if (ia == accounts_.end() || ib == accounts_.end() || a == b) {
        return;
    }
    if (scoreA > 0.75) { 
        ia->second.stats.wins++; 
        ib->second.stats.losses++; 
    }
    else if (scoreA < 0.25) { 
        ia->second.stats.losses++; 
        ib->second.stats.wins++; 
    }
    else {
        ia->second.stats.draws++;
        ib->second.stats.draws++;
    }
    writeUsers();
}