#include "game.h"

const char* moveErrorCode(MoveResult r) {
    switch (r) {
        case MoveResult::Ok: return "OK";
        case MoveResult::GameOver: return "GAME_OVER";
        case MoveResult::OutOfBounds: return "BAD_CELL";
        case MoveResult::NotYourTurn: return "NOT_YOUR_TURN";
        case MoveResult::CellTaken: return "CELL_TAKEN";
    }
    return "UNKNOWN";
}

bool Game::validParams(int n, int k) {
    return n >= kMinSize && n <= kMaxSize && k >= 3 && k <= n;
}

Game::Game(int n, int k) : n_(n), k_(k), cells_(size_t(n) * n, '_') {}

MoveResult Game::move(char sym, int row, int col) {
    if (finished()) return MoveResult::GameOver;
    if (!inside(row, col)) return MoveResult::OutOfBounds;
    if (sym != turn_) return MoveResult::NotYourTurn;
    if (at(row, col) != '_') return MoveResult::CellTaken;

    cells_[row * n_ + col] = sym;
    lastRow_ = row;
    lastCol_ = col;
    checkAfterMove(row, col);
    if (!finished()) turn_ = (turn_ == 'X' ? 'O' : 'X');
    return MoveResult::Ok;
}

void Game::checkAfterMove(int row, int col) {
    const char sym = at(row, col);
    const int dirs[4][2] = {{0, 1}, {1, 0}, {1, 1}, {1, -1}};

    for (const auto& d : dirs) {
        int r = row, c = col;
        while (inside(r - d[0], c - d[1]) && at(r - d[0], c - d[1]) == sym) {
            r -= d[0];
            c -= d[1];
        }
        std::vector<std::pair<int, int>> line;
        while (inside(r, c) && at(r, c) == sym) {
            line.emplace_back(r, c);
            r += d[0];
            c += d[1];
        }
        if (int(line.size()) >= k_) {
            winner_ = sym;
            winLine_ = std::move(line);
            return;
        }
    }
    if (cells_.find('_') == std::string::npos) draw_ = true;
}

std::string Game::boardLine() const {
    std::string s = "BOARD";
    for (char ch : cells_) {
        s += ' ';
        s += ch;
    }
    return s;
}