#include <csignal>

#include "Solver.hpp"

std::atomic<bool> g_running(true);

void signal_handler(int) {
    g_running = false;
}

int main() {
    std::signal(SIGINT, signal_handler);

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