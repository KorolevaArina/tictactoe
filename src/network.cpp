// network.cpp — select()-цикл с неблокирующими сокетами.
#include "network.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <iostream>
#include <vector>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0 // на старых macOS нет; SIGPIPE всё равно игнорируется в main
#endif

static bool setNonBlocking(int fd)
{
    int flags = ::fcntl(fd, F_GETFL, 0);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

Network::~Network()
{
    for (auto &[fd, c] : conns_)
        ::close(fd);
    if (listenFd_ >= 0)
        ::close(listenFd_);
}

bool Network::listen(int port)
{
    listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0)
    {
        std::perror("socket");
        return false;
    }
    int opt = 1;
    ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(uint16_t(port));
    if (::bind(listenFd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
    {
        std::perror("bind");
        return false;
    }
    if (::listen(listenFd_, 64) < 0)
    {
        std::perror("listen");
        return false;
    }
    return setNonBlocking(listenFd_);
}

void Network::run(NetHandler &handler, const volatile std::sig_atomic_t &stop)
{
    while (!stop)
    {
        fd_set rfds, wfds;
        FD_ZERO(&rfds);
        FD_ZERO(&wfds);
        FD_SET(listenFd_, &rfds);
        int maxFd = listenFd_;
        for (auto &[fd, c] : conns_)
        {
            if (!c.closing)
                FD_SET(fd, &rfds);
            if (!c.writeBuf.empty())
                FD_SET(fd, &wfds);
            if (fd > maxFd)
                maxFd = fd;
        }

        timeval tv{1, 0}; // таймаут 1 с — чтобы регулярно проверять heartbeat
        int ret = ::select(maxFd + 1, &rfds, &wfds, nullptr, &tv);
        if (ret < 0)
        {
            if (errno == EINTR)
                continue; // прервано сигналом (Ctrl+C)
            std::perror("select");
            break;
        }

        if (ret > 0)
        {
            // Новые клиенты добавляются в конец цикла, поэтому здесь
            // обрабатываем только тех, кто был в наборе для select().
            for (auto &[fd, c] : conns_)
            {
                if (!c.dead && !c.closing && FD_ISSET(fd, &rfds))
                    readFrom(c, handler);
                if (!c.dead && FD_ISSET(fd, &wfds))
                    flush(c);
            }
            if (FD_ISSET(listenFd_, &rfds))
                acceptClients();
        }

        auto now = Clock::now();
        checkHeartbeat(now);
        reap(handler, now);
    }
}

void Network::acceptClients()
{
    while (true)
    {
        int fd = ::accept(listenFd_, nullptr, nullptr);
        if (fd < 0)
        {
            if (errno == EINTR)
                continue;
            return; // EAGAIN — очередь пуста; прочие ошибки игнорируем
        }
        // select() умеет работать только с fd < FD_SETSIZE (обычно 1024).
        if (fd >= FD_SETSIZE || !setNonBlocking(fd))
        {
            ::close(fd);
            continue;
        }
        Connection c;
        c.fd = fd;
        c.lastActive = Clock::now();
        conns_[fd] = std::move(c);
    }
}

void Network::readFrom(Connection &c, NetHandler &handler)
{
    bool eof = false;
    char buf[4096];
    while (true)
    {
        ssize_t n = ::recv(c.fd, buf, sizeof(buf), 0);
        if (n > 0)
        {
            c.readBuf.append(buf, size_t(n));
            continue;
        }
        if (n == 0)
        {
            eof = true;
            break;
        } // клиент закрыл соединение (на запись)
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            break;
        c.dead = true; // ошибка сокета
        return;
    }

    if (!c.readBuf.empty())
    {
        // Любые данные от клиента = он жив.
        c.lastActive = Clock::now();
        c.awaitingPong = false;
    }

    // Строки, пришедшие вместе с EOF, тоже обрабатываем (напр. "MOVE 1 1\nQUIT\n").
    size_t pos;
    while (!c.dead && !c.closing && (pos = c.readBuf.find('\n')) != std::string::npos)
    {
        std::string line = c.readBuf.substr(0, pos);
        c.readBuf.erase(0, pos + 1);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty() || line == "PONG")
            continue; // PONG нужен только для heartbeat
        handler.onLine(c.fd, line);
    }
    if (c.readBuf.size() > kMaxLine)
        c.dead = true; // слишком длинная строка без '\n'
    if (eof && !c.dead)
    {
        // Ответы на последние команды ещё могут быть в writeBuf — сначала дошлём их.
        // Если клиент закрыл сокет полностью, send() вернёт ошибку и соединение станет dead.
        if (c.writeBuf.empty())
        {
            c.dead = true;
        }
        else if (!c.closing)
        {
            c.closing = true;
            c.closingSince = Clock::now();
        }
    }
}

void Network::flush(Connection &c)
{
    while (!c.writeBuf.empty())
    {
        ssize_t n = ::send(c.fd, c.writeBuf.data(), c.writeBuf.size(), MSG_NOSIGNAL);
        if (n > 0)
        {
            c.writeBuf.erase(0, size_t(n));
        }
        else if (n < 0 && errno == EINTR)
        {
            continue;
        }
        else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            return; // буфер ядра полон — остаток дошлём после следующего select()
        }
        else
        {
            c.dead = true;
            return;
        }
    }
}

void Network::send(int fd, const std::string &line)
{
    auto it = conns_.find(fd);
    if (it == conns_.end() || it->second.dead)
        return;
    Connection &c = it->second;
    if (c.writeBuf.size() + line.size() + 1 > kMaxWriteBuf)
    {
        c.dead = true; // клиент не читает данные — отключаем
        return;
    }
    c.writeBuf += line;
    c.writeBuf += '\n';
}

void Network::closeAfterFlush(int fd)
{
    auto it = conns_.find(fd);
    if (it == conns_.end() || it->second.closing)
        return;
    it->second.closing = true;
    it->second.closingSince = Clock::now();
}

void Network::checkHeartbeat(Clock::time_point now)
{
    using std::chrono::seconds;
    for (auto &[fd, c] : conns_)
    {
        if (c.dead || c.closing)
            continue;
        if (!c.awaitingPong && now - c.lastActive > seconds(kPingAfterSec))
        {
            send(fd, "PING");
            c.awaitingPong = true;
            c.pongSentAt = now;
        }
        else if (c.awaitingPong && now - c.pongSentAt > seconds(kPongTimeoutSec))
        {
            c.dead = true; // партия будет сохранена в onDisconnect
        }
    }
}

void Network::reap(NetHandler &handler, Clock::time_point now)
{
    std::vector<int> toClose;
    for (auto &[fd, c] : conns_)
    {
        bool flushed = c.closing && (c.writeBuf.empty() || now - c.closingSince > std::chrono::seconds(2));
        if (c.dead || flushed)
            toClose.push_back(fd);
    }
    for (int fd : toClose)
    {
        handler.onDisconnect(fd); // может отправить сообщения другим клиентам
        ::close(fd);
        conns_.erase(fd);
    }
}

void Network::closeAll(NetHandler &handler)
{
    for (auto &[fd, c] : conns_)
    {
        handler.onDisconnect(fd);
        ::close(fd);
    }
    conns_.clear();
}
