#ifndef PROXY_MANAGER_HPP
#define PROXY_MANAGER_HPP

#include <string>
#include <vector>
#include <fstream>
#include <atomic>
#include <mutex>
#include <algorithm>

#include "NetworkClient.hpp"

class ProxyManager {
public:
    ProxyManager() : current_index(0) {}

    bool load(const std::string& filename) {
        std::lock_guard<std::mutex> lock(mtx);

        std::ifstream f(filename);
        if (!f.is_open()) return false;

        std::string line;
        while (std::getline(f, line)) {
            parseAndAdd(line);
        }
        return !proxies.empty();
    }

    bool loadFromUrl(NetworkClient* client, const std::string& url) {
        if (!client) return false;

        auto response = client->request(url, "GET");

        if (!response.success || response.status_code != 200) return false;

        std::lock_guard<std::mutex> lock(mtx);

        std::stringstream ss(response.body);
        std::string line;
        while (std::getline(ss, line)) parseAndAdd(line);

        return !proxies.empty();
    }

    template<typename T>
    void optimizeProxies(std::atomic<bool>* running, NetworkClient* client, unsigned int concurrent_checks, T* inst, void(T::*statsCallback)(double speed, long long elapsed, size_t proxy, size_t total, size_t requests)) {
        if (!client) return;

        std::vector<std::string> unique_proxies;
        {
            std::lock_guard<std::mutex> lock(mtx);
            if (proxies.empty()) return;
            std::sort(proxies.begin(), proxies.end());
            proxies.erase(std::unique(proxies.begin(), proxies.end()), proxies.end());
            unique_proxies = proxies;
        }

        auto start = std::chrono::steady_clock::now();

        std::vector<std::string> optimized_proxies;
        std::mutex opt_mtx;
        std::vector<std::future<void>> tasks;

        static const std::vector<std::string> schemes = {"socks5://", "socks4://", "https://", "http://"};

        size_t total = unique_proxies.size();
        std::atomic<size_t> proxy = 0, requests = 0;
        for (const auto& raw_proxy : unique_proxies) {
            if (running && !*running) break;

            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start).count();
            double elapsed_dbl = std::chrono::duration<double>(now - start).count();
            double speed = (elapsed_dbl > 0) ? (proxy / elapsed_dbl) : 0.0;
            (inst->*statsCallback)(speed, elapsed, proxy, total, requests);

            while (tasks.size() >= concurrent_checks) {
                if (running && !*running) break;

                bool removed = false;
                for (size_t i = 0; i < tasks.size(); ) {
                    if (tasks[i].wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
                        tasks[i].get();
                        if (i < tasks.size() - 1)
                            tasks[i] = std::move(tasks.back());

                        tasks.pop_back();
                        removed = true;
                    } else ++i;
                }
                if (!removed) std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            if (running && !*running) break;

            tasks.push_back(std::async(std::launch::async, [&, raw_proxy]() {
                std::vector<std::string> candidates;
                if (raw_proxy.find("://") != std::string::npos)
                    candidates.push_back(raw_proxy);
                else
                    for (const auto& s : schemes) candidates.push_back(s + raw_proxy);

                for (const auto& p : candidates) {
                    if (running && !*running) break;

                    ++requests;
                    auto resp = client->request("http://www.google.com", "GET", "", {}, 5L, p);
                    --requests;
                    if (resp.success && resp.status_code == 200) {
                        std::lock_guard<std::mutex> lock(opt_mtx);
                        optimized_proxies.push_back(p);
                        break;
                    }
                }
                ++proxy;
            }));
        }

        for (auto& task : tasks) task.get();

        {
            std::lock_guard<std::mutex> lock(mtx);
            proxies = optimized_proxies;
            current_index = 0;
        }

        std::ofstream out("proxies_optimized.txt");
        if (out.is_open()) {
            for (const auto& p : proxies) out << p << "\n";
        }
    }

    size_t getCount() const {
        std::lock_guard<std::mutex> lock(mtx);
        return proxies.size();
    }

    size_t getIndex() const {
        return current_index;
    }

    std::string getNext() {
        std::lock_guard<std::mutex> lock(mtx);

        if (proxies.empty()) return "";

        std::string ret = proxies[current_index];
        if (current_index + 1 >= proxies.size()) current_index = 0;
        else ++current_index;
        return ret;
    }

private:
    mutable std::mutex mtx;
        std::vector<std::string> proxies;
    std::atomic<size_t> current_index;

    void parseAndAdd(std::string line) {
        size_t first = line.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return;
        size_t last = line.find_last_not_of(" \t\r\n");
        std::string proxy = line.substr(first, (last - first + 1));

        if (proxy.find("://") == std::string::npos && proxy.find('@') == std::string::npos) {
            size_t c1 = proxy.find(':');
            size_t c2 = (c1 != std::string::npos) ? proxy.find(':', c1 + 1) : std::string::npos;
            size_t c3 = (c2 != std::string::npos) ? proxy.find(':', c2 + 1) : std::string::npos;

            if (c3 != std::string::npos && proxy.find(':', c3 + 1) == std::string::npos) {
                std::string ip = proxy.substr(0, c1);
                std::string port = proxy.substr(c1 + 1, c2 - c1 - 1);
                std::string user = proxy.substr(c2 + 1, c3 - c2 - 1);
                std::string pass = proxy.substr(c3 + 1);
                proxy = user + ":" + pass + "@" + ip + ":" + port;
            }
        }
        proxies.push_back(proxy);
    }
};

#endif