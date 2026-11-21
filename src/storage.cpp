#include "storage.h"
#include "game.h"

#include <algorithm>
#include <cctype>
#include <cmath>
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

// ---------- Elo ----------

double elo_expected(int player, int opponent) {
    return 1.0 / (1.0 + std::pow(10.0, (opponent - player) / 400.0));
}

int elo_updated(int player, int opponent, double score) {
    return int(std::lround(player + kEloK * (score - elo_expected(player, opponent))));
}

Storage::Storage(std::string usersFile, std::string savesFile)
    : usersFile_(std::move(usersFile)), savesFile_(std::move(savesFile)) {
    loadUsers();
    loadSaves();
}

// ---------- users.txt ----------

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
        if (!(ss >> a.stats.elo)) a.stats.elo = kInitialElo;
        if (valid_username(user) && valid_password(a.pass)) {
            accounts_[user] = a;
        }
    }
}


void Storage::writeUsers() const {
    std::ostringstream out;
    for (const auto& [user, a] : accounts_) {
        out << user << ' ' << a.pass << ' ' << a.stats.wins << ' ' << a.stats.losses << ' '
            << a.stats.draws << ' ' << a.stats.elo << '\n';
    }
    atomicWrite(usersFile_, out.str());
}


bool Storage::registerUser(const std::string& user, const std::string& pass) {
    if (!valid_username(user) || !valid_password(pass) || accounts_.count(user)) {
        return false;
    }
    Account a;
    a.pass = pass;  // TODO: хэшировать вместо открытого текста
    a.stats.elo = kInitialElo;
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

    const int oldA = ia->second.stats.elo;
    const int oldB = ib->second.stats.elo;

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

    ia->second.stats.elo = elo_updated(oldA, oldB, scoreA);
    ib->second.stats.elo = elo_updated(oldB, oldA, 1.0 - scoreA);
    writeUsers();
}

std::vector<LeaderEntry> Storage::leaderboard(size_t limit) const {
    std::vector<LeaderEntry> v;
    for (const auto& [name, a] : accounts_) v.push_back({ name, a.stats });
    std::sort(v.begin(), v.end(), [](const LeaderEntry& x, const LeaderEntry& y) {
        if (x.stats.elo != y.stats.elo) return x.stats.elo > y.stats.elo;
        if (x.stats.wins != y.stats.wins) return x.stats.wins > y.stats.wins;
        return x.name < y.name;
        });
    if (v.size() > limit) v.resize(limit);
    return v;
}

// ---------- saves.txt ----------

void Storage::loadSaves() {
    std::ifstream in(savesFile_);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ss(line);
        SavedGame g;
        std::string turn;
        if (!(ss >> g.p1 >> g.p2 >> turn >> g.n >> g.k >> g.board) || turn.size() != 1) continue;
        g.turn = turn[0];
        Game check;
        if (!valid_username(g.p1) || !valid_username(g.p2) || g.p1 == g.p2 ||
            !Game::restore(g.n, g.k, g.board, g.turn, check))
            continue;
        saves_.push_back(g);
    }
}

void Storage::writeSaves() const {
    std::ostringstream out;
    for (const auto& g : saves_)
        out << g.p1 << ' ' << g.p2 << ' ' << g.turn << ' ' << g.n << ' ' << g.k << ' ' << g.board << '\n';
    atomicWrite(savesFile_, out.str());
}

void Storage::saveGame(const SavedGame& g) {
    saves_.erase(std::remove_if(saves_.begin(), saves_.end(), [&](const SavedGame& s) {
        return s.p1 == g.p1 || s.p2 == g.p1 || s.p1 == g.p2 || s.p2 == g.p2;
        }), saves_.end());
    saves_.push_back(g);
    writeSaves();
}

bool Storage::findSave(const std::string& user, SavedGame& out) const {
    for (const auto& s : saves_)
        if (s.p1 == user || s.p2 == user) { out = s; return true; }
    return false;
}

void Storage::eraseSavesWith(const std::string& user) {
    auto before = saves_.size();
    saves_.erase(std::remove_if(saves_.begin(), saves_.end(), [&](const SavedGame& s) {
        return s.p1 == user || s.p2 == user;
        }), saves_.end());
    if (saves_.size() != before) writeSaves();
}