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

        std::ifstream configFile("conf.json");
        if (!configFile.is_open()) {
            printer.pushLeft("[Config] Error: Could not open conf.json");
            return 1;
        }

        nlohmann::json config;
        try {
            configFile >> config;
        } catch (const nlohmann::json::parse_error& e) {
            printer.pushLeft(std::string("[Config] JSON Parse Error: ") + e.what());
            return 2;
        }

        std::string root = config.value("root", "");

        GameData data;
        auto [code, elms] = data.load(root + config.value("elements", "elements.bin"), root + config.value("recipes", "recipes.bin"));
        printer.pushLeft("[Storage] Loaded " + std::to_string(elms) + " elements.");
        switch(code) {
            case 1:
                printer.pushLeft("[Storage] Warning: ID limit approaching capacity.");
                break;
            case 2:
                if (elms == 0) printer.pushLeft("[Storage] Elements database is corrupted.");
                else printer.pushLeft("[Storage] Recipes database is corrupted.");
                return 3;
        }
        data.initDefaults();

        NetworkClient client;
        if (std::string headers = config.value("headers", ""); headers != "")
            if (size_t count = client.loadHeaders(root + headers); count > 0) printer.pushLeft("[Netwrok] Loaded " + std::to_string(count) + " headers.");
            else printer.pushLeft("[Netwrok] Headers file found, but 0 headers loaded.");
        else printer.pushLeft("[Netwrok] Headers file not provided.");

        ProxyManager proxyManager;
        bool proxiesLoaded = false;

        if (config.contains("proxies") && config["proxies"].is_array())
            for (const auto& path : config["proxies"])
                if (path.is_string()) {
                    printer.pushLeft("[Proxies] Loading proxies from " + root + (std::string)path);
                    if (proxyManager.load(root + (std::string)path)) proxiesLoaded = true;
                    else printer.pushLeft("[Proxies] Failed to load proxies.");
                }

        if (config.contains("proxiesUrl") && config["proxiesUrl"].is_array())
            for (const auto& url : config["proxiesUrl"])
                if (url.is_string()) {
                    printer.pushLeft("[Proxies] Downloading proxies from " + (std::string)url);
                    if (proxyManager.loadFromUrl(&client, (std::string)url)) proxiesLoaded = true;
                    else printer.pushLeft("[Proxies] Failed to download proxies.");
                }

        if (proxiesLoaded) printer.pushLeft("[Proxies] Loaded " + std::to_string(proxyManager.getCount()) + " proxies.");
        else printer.pushLeft("[Proxies] No proxies loaded. Using direct connection.");

        Solver solver(data, client, printer, proxyManager);

        if (g_running) solver.run(&g_running, config.value("workers", 1), config.value("concurrency", 1));
    } catch (const std::string& e) {
        std::cerr << "Fatal error (str): " << e;
        return -1;
    } catch (const std::exception& e) {
        std::cerr << "Fatal error (exc): " << std::string(e.what());
        return -1;
    }

    return 0;
}