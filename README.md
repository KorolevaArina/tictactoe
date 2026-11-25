# Крестики-нолики: клиент-сервер на C++17

Консольная игра N×N (K в ряд) по TCP. Сервер однопоточный, на `select()`.
Аккаунты с SHA-256, Elo-рейтинг, таблица лидеров, сохранение партии при обрыве связи,
heartbeat PING/PONG, лог событий, unit-тесты на Google Test.

## Сборка и запуск

```bash
cmake -S . -B build
cmake --build build --parallel
ctest --test-dir build --output-on-failure   # тесты

./build/server 5555                          # сервер (файлы пишутся в текущую папку)
./build/client 127.0.0.1 5555                # клиент (в другом терминале)
```

Google Test берётся из системы (`find_package`); если его нет, CMake скачивает его
через `FetchContent`. Собрать без тестов: `cmake -S . -B build -DBUILD_TESTS=OFF`.

## Архитектура

```
src/main.cpp ──► Network (src/network.*) ──строки──► Protocol (src/protocol.*) ──► Game    (src/game.*)
                 select, буферы,                     команды, лобби,           └─► Storage (src/storage.*) ──► src/sha256.*
                 PING/PONG                           партии, RESUME                users.txt, saves.txt,
                                                                                   Elo, лидерборд, server.log
```

| Модуль | За что отвечает | Зависит от |
|---|---|---|
| `src/game.h/.cpp` | Поле N×N, ход, проверка победы от последнего хода (4 направления), ничья, восстановление из сохранения, строка `BOARD` | — |
| `src/sha256.h/.cpp` | Своя реализация SHA-256 | — |
| `src/storage.h/.cpp` | `users.txt`, `saves.txt`, формула Elo, лидерборд, `log_event()` | game, sha256 |
| `src/network.h/.cpp` | `select()` с таймаутом 1 с, неблокирующие сокеты, буферы чтения и записи, разбор строк, heartbeat | — |
| `src/protocol.h/.cpp` | Разбор команд и логика лобби: вход, очередь и приглашения, партии, сохранение и возобновление | network, game, storage |
| `src/main.cpp` | Порт, сигналы, связывает модули; при Ctrl+C сохраняет незаконченные партии | всё |
| `client/client.cpp` | Консольный клиент на `poll()` (цветной вывод, маскировка ввода пароля, автоответ `PONG`) | — |
| `tests/test_*.cpp` | Unit-тесты логики игры, SHA-256, Elo и хранилища | ttt_core, GTest |

`game`, `storage` и `sha256` собираются в библиотеку `ttt_core`: от сети они не зависят,
поэтому тесты проверяют их без запуска сервера.
`Network` ничего не знает об игре: каждую полученную строку он передаёт
в интерфейс `NetHandler`, а `Protocol` этот интерфейс реализует.

## Протокол (одна команда = одна строка)

Клиент → сервер:

| Команда | Смысл |
|---|---|
| `REGISTER user pass`, `LOGIN user pass` | аккаунт (если пароль не указан в команде, клиент запросит его скрыто) |
| `TOP`, `STATS [user]`, `WHO` | лидерборд, статистика, кто онлайн |
| `NEWGAME [N K]` | встать в очередь, сыграть с первым, кто выберет те же N K (по умолчанию 3 3) |
| `NEWGAME [N K] opp` | пригласить игрока; ему приходит `INVITE_FROM`, он отвечает `ACCEPT opp` / `DECLINE opp` |
| `CANCEL` | отменить приглашения, очередь и ожидание RESUME |
| `MOVE r c` | ход (с 0) |
| `RESUME` | продолжить сохранённую партию |
| `CHAT text`, `QUIT`, `PONG` | чат, выход, ответ на heartbeat |

Сервер → клиент:
- Статусы и ошибки: `OK ...`, `ERROR <код>`, `PING`, `BYE`
- Игра: `GAME_START opp side N K`, `RESUMED opp side N K`, `BOARD X O _ ...` (N·N символов через пробел), `TURN X`, `LAST r c` (последний ход), `WIN X r,c;r,c;...`, `DRAW`
- Лобби и приглашения: `QUEUED ...`, `INVITE_FROM from N K`, `INVITE_SENT`, `INVITE_DECLINED`, `INVITE_CANCELED`, `OK_DECLINED`, `CHAT_MSG scope from text`
- Статистика и онлайн: `TOP_BEGIN count` / `TOP_ENTRY place name elo w l d` / `TOP_END`, `STATS_INFO user w l d [elo]`, `WHO_BEGIN` / `WHO_USER user status` / `WHO_END`
- Сохранения: `RESUMED_AVAILABLE opp turn`, `NO_SAVE`, `WAIT_OPPONENT opp`, `OPPONENT_LEFT_SAVED`

## Файлы данных

- `users.txt`: `username sha256(password) wins losses draws elo`
- `saves.txt`: `player1 player2 turn N K board`. `player1` играет `X`; `board` — N·N символов `X/O/_`
- `server.log`: `[2025-01-01 12:00:00] LOGIN alice` (а также `REGISTER`, `MOVE`, `RESUME`, `SAVE`, `LOAD_SAVE`, `GAME_END`, `DISCONNECT`)

## Сохранение партии: правила

1. Игрок отключился посреди партии (EOF, ошибка или нет `PONG` в течение 5 с) → партия пишется в `saves.txt`, сопернику уходит `OPPONENT_LEFT_SAVED`.
2. При `LOGIN` сервер отправляет `RESUMED_AVAILABLE opp turn`, если для игрока есть сохранение.
3. `RESUME`: если соперник онлайн, партия продолжается сразу. Если нет, сервер отвечает `WAIT_OPPONENT` и держит партию в памяти, а когда соперник войдёт, игра продолжится автоматически.
4. Новая игра удаляет старые сохранения обоих участников, поэтому у игрока всегда не больше одного сохранения.