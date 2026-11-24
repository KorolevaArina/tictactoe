# Крестики-нолики (клиент-сервер)

Консольная сетевая игра в крестики-нолики на поле N×N (победа при K в ряд,
`3 <= N <= 10`, `3 <= K <= N`). Сервер однопоточный на `select()`, клиент
работает поверх `poll()`.

## Сборка (временная)

```bash
g++ -std=c++17 -O2 -Wall -Wextra server.cpp game.cpp storage.cpp -o server
g++ -std=c++17 -O2 -Wall -Wextra client.cpp -o client
./server 5555
./client 127.0.0.1 5555
