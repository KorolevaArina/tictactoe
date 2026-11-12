#include "game.h"

#include <iostream>
#include <utility>
#include <vector>

int main() {
    Game g(3, 3);
    std::vector<std::pair<int, int>> moves = {{0, 0}, {1, 0}, {0, 1}, {1, 1}, {0, 2}};
    for (auto [r, c] : moves) {
        MoveResult res = g.move(g.turn(), r, c);
        std::cout << moveErrorCode(res) << " -> " << g.boardLine() << "\n";
    }
    std::cout << "Winner: " << g.winner() << "\n";
    return 0;
}