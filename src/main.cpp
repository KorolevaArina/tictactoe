// Запуск: ./server [port]   (по умолчанию 5555)
#include "network.h"
#include "protocol.h"
#include "storage.h"

#include <csignal>
#include <iostream>
#include <string>

static volatile std::sig_atomic_t g_stop = 0;
static void onSignal(int) { g_stop = 1; }

int main(int argc, char **argv)
{
    int port = 5555;
    if (argc >= 2)
    {
        try
        {
            port = std::stoi(argv[1]);
        }
        catch (...)
        {
            port = 0;
        }
        if (port < 1 || port > 65535)
        {
            std::cerr << "usage: " << argv[0] << " [port 1..65535]\n";
            return 1;
        }
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    std::signal(SIGPIPE, SIG_IGN); // запись в закрытый сокет не должна убивать процесс

    Network net;
    if (!net.listen(port))
        return 1;
    Storage storage("users.txt", "saves.txt");
    Protocol protocol(net, storage);

    std::cout << "Server listening on port " << port << " (Ctrl+C to stop)\n";
    log_event("SERVER_START port=" + std::to_string(port));
    net.run(protocol, g_stop);

    // Остановка: отключаем всех — незаконченные партии сохранятся в saves.txt.
    net.closeAll(protocol);
    log_event("SERVER_STOP");
    std::cout << "Server stopped\n";
    return 0;
}
