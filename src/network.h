// network.h — однопоточный сетевой цикл на select(): accept, буферы чтения/записи,
// разбиение потока на строки, heartbeat (PING/PONG).
// О командах игры модуль ничего не знает — строки передаются в NetHandler.
#pragma once

#include <chrono>
#include <csignal>
#include <map>
#include <string>

using Clock = std::chrono::steady_clock;

// Интерфейс обработчика строк (реализован в protocol.cpp).
class NetHandler
{
public:
    virtual ~NetHandler() = default;
    virtual void onLine(int fd, const std::string &line) = 0;
    // Вызывается ровно один раз перед закрытием сокета (EOF, ошибка, таймаут, QUIT).
    virtual void onDisconnect(int fd) = 0;
};

struct Connection
{
    int fd = -1;
    std::string readBuf;  // принятые, но ещё не разобранные байты
    std::string writeBuf; // то, что send() ещё не успел отправить
    Clock::time_point lastActive;
    bool awaitingPong = false;
    Clock::time_point pongSentAt;

    bool dead = false;    // закрыть на ближайшей итерации
    bool closing = false; // QUIT: дослать writeBuf и закрыть
    Clock::time_point closingSince;
};

class Network
{
public:
    static constexpr int kPingAfterSec = 10;  // тишина -> PING
    static constexpr int kPongTimeoutSec = 5; // нет ответа на PING -> соединение мёртвое
    static constexpr size_t kMaxLine = 64 * 1024;
    static constexpr size_t kMaxWriteBuf = 1024 * 1024;

    Network() = default;
    ~Network();
    Network(const Network &) = delete;
    Network &operator=(const Network &) = delete;

    bool listen(int port);
    // Главный цикл; выходит, когда stop != 0.
    void run(NetHandler &handler, const volatile std::sig_atomic_t &stop);
    // Закрыть все соединения (с вызовом onDisconnect) — при остановке сервера.
    void closeAll(NetHandler &handler);

    // Поставить строку в очередь отправки (добавляется '\n').
    void send(int fd, const std::string &line);
    // Дослать очередь и закрыть соединение (команда QUIT).
    void closeAfterFlush(int fd);

private:
    void acceptClients();
    void readFrom(Connection &c, NetHandler &handler);
    void flush(Connection &c);
    void checkHeartbeat(Clock::time_point now);
    void reap(NetHandler &handler, Clock::time_point now);

    int listenFd_ = -1;
    std::map<int, Connection> conns_;
};
