#ifndef _WIN32
#include <csignal>
#include <unistd.h>
#endif

#include "Solver.hpp"

std::atomic<bool> g_running(true);

#ifdef _WIN32
BOOL WINAPI WindowsCtrlHandler(DWORD fdwCtrlType) {
    switch (fdwCtrlType) {
    case CTRL_C_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        g_running = false;
        return TRUE; 
    default:
        return FALSE;
    }
}
#else
void PosixSignalHandler(int) {
    g_running = false;
}
#endif

int main() {
#ifdef _WIN32
    if (!SetConsoleCtrlHandler(WindowsCtrlHandler, TRUE)) {
        std::cerr << "[System] Error: Could not set control handler." << std::endl;
    }
#else
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = PosixSignalHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGHUP, &sa, nullptr);
#endif

    try
    {
        Printer printer; 

        // 1. Setup Data
        GameData data;
        auto [code, elms] = data.load();
        printer.pushLeft("[Storage] Loaded " + std::to_string(elms) + " elements.");
        switch(code) {
            case 1:
                printer.pushLeft("[Storage] Warning: ID limit approaching capacity.");
                break;
            case 2:
                if (elms == 0) printer.pushLeft("[Storage] Elements database is corrupted.");
                else printer.pushLeft("[Storage] Recipes database is corrupted.");
                return 0;
        }
        data.initDefaults();

        // 2. Setup Network
        NetworkClient client;

        // 3. Setup ProxyManager
        ProxyManager proxyManager;
        if (proxyManager.load("proxies.txt")) {
            printer.pushLeft("[Config] Loaded " + std::to_string(proxyManager.getCount()) + " proxies.");
        } else {
            printer.pushLeft("[Config] No proxies found in proxies.txt. Using direct connection.");
        }

        auto getNum = [&](const std::string& text) {
            unsigned short num = 0;
            while (num == 0 && g_running) {
                if (!std::cin.good()) std::this_thread::sleep_for(std::chrono::milliseconds(50));
                unsigned long conv = strtoul(printer.prompt(text).c_str(), NULL, 0);
                if (conv <= std::numeric_limits<unsigned short>::max()) num = conv;
            }
            return num;
        };

        // 4. Start Solver
        Solver solver(data, client, printer, proxyManager);

        unsigned short workers = getNum("Number of workers: ");
        unsigned short concurrency = getNum("Concurrency: ");
        if (g_running) solver.run(&g_running, workers, concurrency);
    } catch (const std::string& e) {
        std::cout << "Fatal error (str): " << e;
    } catch (const std::exception& e) {
        std::cout << "Fatal error (exc): " << std::string(e.what());
    }

    return 0;
}