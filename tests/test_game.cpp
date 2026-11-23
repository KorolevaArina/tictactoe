// test_game.cpp — тесты правил игры (game.cpp).
#include "game.h"

#include <gtest/gtest.h>

#include <utility>
#include <vector>

// Делает ходы по очереди X, O, X, ... и проверяет, что каждый ход принят.
static void play(Game& g, const std::vector<std::pair<int, int>>& moves) {
    for (const auto& [r, c] : moves) {
        ASSERT_EQ(g.move(g.turn(), r, c), MoveResult::Ok) << "move " << r << "," << c;
    }
}

// ---------- N=3, K=3 ----------

TEST(Game3x3, HorizontalWin) {
    Game g(3, 3);
    play(g, {{0, 0}, {1, 0}, {0, 1}, {1, 1}, {0, 2}});
    EXPECT_EQ(g.winner(), 'X');
    EXPECT_TRUE(g.finished());
    EXPECT_FALSE(g.isDraw());
    EXPECT_EQ(g.winLine().size(), 3u);
}

TEST(Game3x3, VerticalWin) {
    Game g(3, 3);
    play(g, {{0, 0}, {0, 1}, {1, 0}, {1, 1}, {2, 2}, {2, 1}});
    EXPECT_EQ(g.winner(), 'O');
}

TEST(Game3x3, MainDiagonalWin) {
    Game g(3, 3);
    play(g, {{0, 0}, {0, 1}, {1, 1}, {0, 2}, {2, 2}});
    EXPECT_EQ(g.winner(), 'X');
}

TEST(Game3x3, AntiDiagonalWin) {
    Game g(3, 3);
    play(g, {{0, 2}, {0, 0}, {1, 1}, {0, 1}, {2, 0}});
    EXPECT_EQ(g.winner(), 'X');
}

TEST(Game3x3, WinDetectedWhenLastMoveInMiddleOfLine) {
    Game g(3, 3);
    play(g, {{0, 0}, {1, 0}, {0, 2}, {1, 1}, {0, 1}});  // последний ход — в центр линии
    EXPECT_EQ(g.winner(), 'X');
}

TEST(Game3x3, DrawOnFullBoard) {
    // X O X
    // X O O
    // O X X
    Game g(3, 3);
    play(g, {{0, 0}, {0, 1}, {0, 2}, {1, 1}, {1, 0}, {1, 2}, {2, 1}, {2, 0}, {2, 2}});
    EXPECT_TRUE(g.isDraw());
    EXPECT_EQ(g.winner(), '_');
    EXPECT_TRUE(g.finished());
}

TEST(Game3x3, NoWinnerInProgress) {
    Game g(3, 3);
    play(g, {{0, 0}, {1, 1}});
    EXPECT_FALSE(g.finished());
    EXPECT_EQ(g.turn(), 'X');
}

// ---------- N=5, K=4 ----------

TEST(Game5x5K4, ThreeInRowIsNotWin) {
    Game g(5, 4);
    play(g, {{0, 0}, {4, 4}, {0, 1}, {4, 3}, {0, 2}});
    EXPECT_FALSE(g.finished());
}

TEST(Game5x5K4, HorizontalWin) {
    Game g(5, 4);
    play(g, {{2, 1}, {0, 0}, {2, 2}, {0, 1}, {2, 3}, {0, 2}, {2, 4}});
    EXPECT_EQ(g.winner(), 'X');
    EXPECT_EQ(g.winLine().size(), 4u);
}

TEST(Game5x5K4, VerticalWin) {
    Game g(5, 4);
    play(g, {{0, 0}, {1, 4}, {0, 1}, {2, 4}, {0, 3}, {3, 4}, {4, 0}, {4, 4}});
    EXPECT_EQ(g.winner(), 'O');
}

TEST(Game5x5K4, DiagonalWin) {
    Game g(5, 4);
    play(g, {{1, 1}, {0, 4}, {2, 2}, {1, 4}, {3, 3}, {2, 4}, {4, 4}});
    EXPECT_EQ(g.winner(), 'X');
}

TEST(Game5x5K4, AntiDiagonalWin) {
    Game g(5, 4);
    play(g, {{0, 4}, {0, 0}, {1, 3}, {1, 0}, {2, 2}, {3, 0}, {3, 1}});
    EXPECT_EQ(g.winner(), 'X');
}

// ---------- Невалидные ходы ----------

TEST(GameInvalid, CellTaken) {
    Game g(3, 3);
    ASSERT_EQ(g.move('X', 1, 1), MoveResult::Ok);
    EXPECT_EQ(g.move('O', 1, 1), MoveResult::CellTaken);
    EXPECT_EQ(g.turn(), 'O');  // ход не перешёл
}

TEST(GameInvalid, NotYourTurn) {
    Game g(3, 3);
    EXPECT_EQ(g.move('O', 0, 0), MoveResult::NotYourTurn);
    ASSERT_EQ(g.move('X', 0, 0), MoveResult::Ok);
    EXPECT_EQ(g.move('X', 0, 1), MoveResult::NotYourTurn);
}

TEST(GameInvalid, OutOfBounds) {
    Game g(3, 3);
    EXPECT_EQ(g.move('X', 3, 0), MoveResult::OutOfBounds);
    EXPECT_EQ(g.move('X', 0, -1), MoveResult::OutOfBounds);
    EXPECT_EQ(g.move('X', -1, 5), MoveResult::OutOfBounds);
    EXPECT_EQ(g.cells(), std::string(9, '_'));
}

TEST(GameInvalid, MoveAfterGameOver) {
    Game g(3, 3);
    play(g, {{0, 0}, {1, 0}, {0, 1}, {1, 1}, {0, 2}});
    EXPECT_EQ(g.move('O', 2, 2), MoveResult::GameOver);
}

// ---------- Параметры, формат, восстановление ----------

TEST(GameParams, ValidParams) {
    EXPECT_TRUE(Game::validParams(3, 3));
    EXPECT_TRUE(Game::validParams(5, 4));
    EXPECT_TRUE(Game::validParams(10, 10));
    EXPECT_FALSE(Game::validParams(2, 2));
    EXPECT_FALSE(Game::validParams(5, 6));
    EXPECT_FALSE(Game::validParams(11, 5));
    EXPECT_FALSE(Game::validParams(5, 2));
}

TEST(GameFormat, BoardLine) {
    Game g(3, 3);
    g.move('X', 0, 0);
    g.move('O', 0, 1);
    EXPECT_EQ(g.boardLine(), "BOARD X O _ _ _ _ _ _ _");
}

TEST(GameRestore, ContinuesFromSavedState) {
    Game g;
    ASSERT_TRUE(Game::restore(3, 3, "XX_OO____", 'X', g));
    EXPECT_EQ(g.move('X', 0, 2), MoveResult::Ok);
    EXPECT_EQ(g.winner(), 'X');
}

TEST(GameRestore, RejectsBadData) {
    Game g;
    EXPECT_FALSE(Game::restore(3, 3, "XX_", 'X', g));        // не та длина
    EXPECT_FALSE(Game::restore(3, 3, "XX_OO___Z", 'X', g));  // плохой символ
    EXPECT_FALSE(Game::restore(3, 3, "_________", 'Q', g));  // плохой turn
    EXPECT_FALSE(Game::restore(3, 4, "_________", 'X', g));  // K > N
}


