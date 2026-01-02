#ifndef PROXY_MANAGER_HPP
#define PROXY_MANAGER_HPP

#include <vector>
#include <atomic>
#include <mutex>
#include <algorithm>

#include "NetworkClient.hpp"

class ProxyManager {
public:
    ProxyManager() : loop(0), current_index(0) {}

    bool load(const std::string& filename) {
        std::lock_guard<std::mutex> lock(proxy_mutex);

        std::ifstream f(filename);
        if (!f.is_open()) return false;

        std::string line;
        size_t prevsz = proxies.size();
        while (std::getline(f, line)) parseAndAdd(line);

        finalize();
        return proxies.size() - prevsz > 0;
    }

    bool loadFromUrl(NetworkClient* client, const std::string& url) {
        if (!client) return false;

        std::vector<std::string> headers = {};
        auto response = client->request(url, "GET", "", &headers);

        if (!response.success || response.status_code != 200) return false;

        std::lock_guard<std::mutex> lock(proxy_mutex);

        std::stringstream ss(response.body);
        std::string line;
        size_t prevsz = proxies.size();
        while (std::getline(ss, line)) parseAndAdd(line);

        finalize();
        return proxies.size() - prevsz > 0;
    }

    void report(const std::string& proxy) {
        if (!reporting) return;

        std::lock_guard<std::mutex> lock(proxy_mutex);

        auto it = std::lower_bound(proxies.begin(), proxies.end(), proxy,
            [](const ProxyEntry& entry, const std::string& val) {
                return entry.url < val;
            });

        if (it != proxies.end() && it->url == proxy) {
            it->failures++;

            if (it->failures >= 18) {
                size_t index = std::distance(proxies.begin(), it);

                if (index < current_index && current_index > 0) current_index--;
                proxies.erase(it);
                if (current_index >= proxies.size()) current_index = 0;
            }
        }
    }

    std::string getNext() {
        std::lock_guard<std::mutex> lock(proxy_mutex);

        if (proxies.empty()) return "";

        std::string ret = proxies[current_index].url;
        if (++current_index >= proxies.size()) {
            current_index = 0;
            if (reporting.load(std::memory_order_relaxed) && ++loop == 20) {
                for (auto& entry : proxies) entry.failures = 0;
                reporting.store(false, std::memory_order_relaxed);
            }
        }
        return ret;
    }

    size_t getCount() const {
        std::lock_guard<std::mutex> lock(proxy_mutex);

        return proxies.size();
    }

    size_t getIndex() const {
        return current_index;
    }

private:
    struct ProxyEntry {
        std::string url;
        unsigned char failures = 0;

        bool operator<(const ProxyEntry& other) const {
            return url < other.url;
        }
    };

    mutable std::mutex proxy_mutex;
        std::vector<ProxyEntry> proxies;
        size_t loop;
    std::atomic<size_t> current_index;
    std::atomic<bool> reporting{true};

    void finalize() {
        if (proxies.empty()) return;

        std::sort(proxies.begin(), proxies.end());
        auto last = std::unique(proxies.begin(), proxies.end(),
            [](const ProxyEntry& a, const ProxyEntry& b) {
                return a.url == b.url;
            });

        proxies.erase(last, proxies.end());

        if (current_index >= proxies.size()) current_index = 0;
    }

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
        if (proxy.find("://") == std::string::npos) proxy = "http://" + proxy;
        proxies.push_back({proxy, 0});
    }
};

#endif