// game.h (v1)
#pragma once

#include <string>
#include <utility>
#include <vector>

enum class MoveResult {
    Ok,
    GameOver,     // партия уже закончена
    OutOfBounds,  // координаты вне поля
    NotYourTurn,  // ходит не тот символ
    CellTaken     // клетка занята
};

// Код ошибки для протокола: "ERROR <code>".
const char* moveErrorCode(MoveResult r);

class Game {
public:
    static constexpr int kMinSize = 3;
    static constexpr int kMaxSize = 10;

    // 3 <= N <= 10, 3 <= K <= N.
    static bool validParams(int n, int k);

    explicit Game(int n = 3, int k = 3);

    // Ход символом sym ('X' или 'O') в клетку (row, col), нумерация с 0.
    MoveResult move(char sym, int row, int col);

    int size() const { return n_; }
    int winLength() const { return k_; }
    char turn() const { return turn_; }
    const std::string& cells() const { return cells_; }
    int lastRow() const { return lastRow_; }
    int lastCol() const { return lastCol_; }

    char winner() const { return winner_; }  // 'X', 'O' или '_'
    bool isDraw() const { return draw_; }
    bool finished() const { return winner_ != '_' || draw_; }
    const std::vector<std::pair<int, int>>& winLine() const { return winLine_; }

    // "BOARD X O _ _ X _ _ _ _" — N*N символов через пробел.
    std::string boardLine() const;

private:
    char at(int r, int c) const { return cells_[r * n_ + c]; }
    bool inside(int r, int c) const { return r >= 0 && r < n_ && c >= 0 && c < n_; }
    void checkAfterMove(int row, int col);

    int n_, k_;
    std::string cells_;
    char turn_ = 'X';
    int lastRow_ = -1, lastCol_ = -1;
    char winner_ = '_';
    bool draw_ = false;
    std::vector<std::pair<int, int>> winLine_;
};