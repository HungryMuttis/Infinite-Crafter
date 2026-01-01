#ifndef PROXY_MANAGER_HPP
#define PROXY_MANAGER_HPP

#include <string>
#include <vector>
#include <fstream>
#include <atomic>
#include <mutex>
#include <algorithm>

class ProxyManager {
public:
    ProxyManager() : current_index(0) {}

    bool load(const std::string& filename) {
        std::lock_guard<std::mutex> lock(mtx);
        std::ifstream f(filename);
        if (!f.is_open()) return false;
        
        proxies.clear();
        std::string line;
        while (std::getline(f, line)) {
            size_t first = line.find_first_not_of(" \t\r\n");
            if (first == std::string::npos) continue;

            size_t last = line.find_last_not_of(" \t\r\n");
            proxies.push_back(line.substr(first, (last - first + 1)));
        }
        current_index = 0;
        return !proxies.empty();
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
};

#endif