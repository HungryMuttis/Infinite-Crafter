#ifndef SOLVER_HPP
#define SOLVER_HPP

#include "NetworkClient.hpp"
#include "Storage.hpp"
#include "Printer.hpp"
#include "ProxyManager.hpp"

#include <iostream>
#include <thread>
#include <chrono>
#include <iomanip>
#include <sstream>    
#include <fstream>
#include <vector>
#include <random>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

class Solver {
public:
    Solver(std::string headersFile, GameData& data, NetworkClient& client, Printer& printer, ProxyManager& proxyManager) : data(data), client(client), printer(printer), proxyManager(proxyManager) {
        loadHeaders(headersFile);
    }

    void run(std::atomic<bool>* running_flag, unsigned short workers, unsigned short concurrency = 1) {
        this->running_flag = running_flag;

        if (concurrency == 0) concurrency = 32;
        std::vector<std::thread> worker_threads;

        for (unsigned short i = 0; i < workers; ++i) worker_threads.push_back(std::thread(&Solver::worker, this, i, concurrency));
        printer.pushLeft("[Solver] Started " + std::to_string(workers) + " workers (Concurrency: " + std::to_string(concurrency) + ").");

        auto start = std::chrono::steady_clock::now();
        auto updateStats = [&]() {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - start).count();
            double elapsed_dbl = std::chrono::duration<double>(now - start).count();
            double speed = (elapsed_dbl > 0) ? (checks / elapsed_dbl) : 0.0;
            printer.setStats(speed, elapsed, proxyManager.getIndex(), data.getElementCount(), num_requests);
        };

        while (*running_flag) { // Stats loop
            updateStats();
            sleep(10); // 500ms
        }

        printer.pushLeft("[Solver] Stopping... Waiting for active requests to finish.");

        std::future<void> joiner = std::async(std::launch::async, [&worker_threads]() {
            for (std::thread& worker : worker_threads) if (worker.joinable()) worker.join();
        });

        while (joiner.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready) updateStats();

        printer.pushLeft("[Solver] All workers exited.");
    }

private:
    GameData& data;
    NetworkClient& client;
    Printer& printer;
    ProxyManager& proxyManager;

    std::atomic<bool>* running_flag;

    std::vector<std::string> headers;

    std::atomic<size_t> checks = 0;
    std::atomic<size_t> num_requests = 0;

    struct ActiveRequest {
        std::future<NetworkClient::Response> future;
        std::pair<uint32_t, uint32_t> comb;
        std::pair<std::string, std::string> names;
        std::string proxy;
    };

    void worker(unsigned short workerId, unsigned short concurrency) {
        std::string workerStr = std::to_string(workerId);
        printer.pushLeft("[Worker-" + workerStr + "] Started.");

        std::vector<ActiveRequest> requests;
        requests.reserve(concurrency);
        
        while (*running_flag || !requests.empty()) {
            bool wait = true;

            auto it = requests.begin();
            while (it != requests.end()) {
                if (it->future.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                    wait = false;
                    --num_requests;
                    NetworkClient::Response response = it->future.get();

                    auto [success, id] = processResponse(response, it->names);

                    if (success) {
                        ++checks;
                        data.addRecipe(it->comb.first, it->comb.second, id);
                        it = requests.erase(it);
                    } else {
                        if (*running_flag) {
                            handleError(id, it->proxy, workerStr, it->names);

                            std::string proxy = proxyManager.getNext();
                            it->proxy = proxy;
                            it->future = createRequest(it->names, proxy);
                            ++num_requests;
                            
                            ++it;
                        } else it = requests.erase(it);
                    }
                } else ++it;
            }

            if (*running_flag && requests.size() < concurrency) {
                std::pair<uint32_t, uint32_t> comb = next();

                if (comb.first != 0) {
                    wait = false;

                    std::pair<std::string, std::string> names = getNames(comb);
                    std::string proxy = proxyManager.getNext();

                    requests.push_back({
                        createRequest(names, proxy),
                        comb,
                        names,
                        proxy
                    });
                    ++num_requests;
                }
            }

            if (wait) {
                if (*running_flag) {
                    if (requests.empty()) sleep(20); // 1s
                    else sleep(1); // 50ms
                } else std::this_thread::sleep_for(std::chrono::milliseconds(50)); // 50ms
            }
        }

        printer.pushLeft("[Worker-" + workerStr + "] Exited.");
    }

    std::pair<bool, size_t> processResponse(const NetworkClient::Response& response, const std::pair<std::string, std::string>& names) {
        if (!response.success) {
            if (response.status_code == 0) return { false, 0 }; // bad proxy
            if (response.status_code == 429) return { false, 1 }; // rate-limited

            logDebugInfo(names.first, names.second, response, "Response not success");
            return { false, 4 };
        }

        if (!response.body.empty() && response.body[0] == '<') return { false, 2 }; // blocked

        try {
            auto j = json::parse(response.body);
            std::string result = j.value("result", "Nothing");
            
            if (result == "Nothing") {
                printer.pushRight(names.first + " + " + names.second + " = Nothing");
                return { true, GameData::NOTHING_ID };
            } else {
                std::string emoji = j.value("emoji", "");
                bool isGlobalNew = j.value("isNew", false);
                
                auto [resultId, isLocalNew] = data.addElement(result, emoji, isGlobalNew);
                printer.pushRight((isLocalNew ? (isGlobalNew ? "[GLOBAL NEW] " : "[NEW] ") : "") + names.first + " + " + names.second + " = " + emoji + " " + result + " (#" + std::to_string(resultId) + ")");
                return { true, resultId };
            }
        } catch (const json::parse_error& e) {
            logDebugInfo(names.first, names.second, response, std::string(e.what()));
            return { false, 3 };
        }
    }

    void handleError(size_t errorId, const std::string& proxy, const std::string& workerStr, const std::pair<std::string, std::string>& names) {
        switch (errorId) {
        case 0:
            // Fix lagging when in proxy deadzones
            std::this_thread::sleep_for(std::chrono::milliseconds(1)); // 1ms
            break;
        case 1:
            printer.pushLeft("[Worker-" + workerStr + "][Network] Rate-limited: " + proxy);
            break;
        case 2:
            printer.pushLeft("[Worker-" + workerStr + "][Network] Blocked (" + proxy + ")");
            break;
        case 3:
            printer.pushLeft("[Worker-" + workerStr + "][Json] JSON Error on " + names.first + "+" + names.second);
            break;
        case 4:
            printer.pushLeft("[Worker-" + workerStr + "][Network] Unknown Error (see debug log)");
            break;
        }
    }

    std::future<NetworkClient::Response> createRequest(const std::pair<std::string, std::string>& names, const std::string& proxy) {
        return client.requestAsync("https://neal.fun/api/infinite-craft/pair?first=" + urlEncode(names.first) + "&second=" + urlEncode(names.second), "GET", "", headers, 10L, proxy);
    }

    std::mutex element_mutex;
        size_t cursor_i = 1, cursor_j = 1;
    std::pair<uint32_t, uint32_t> next() {
        std::lock_guard<std::mutex> lock(element_mutex);
        
        size_t max_elements = data.getElementCount();

        while (cursor_i < max_elements) {
            cursor_j++;
            
            if (cursor_j > cursor_i) {
                cursor_i++;
                cursor_j = 1;
            }

            if (cursor_i >= max_elements) break;
            if (data.getResult((uint32_t)cursor_i, (uint32_t)cursor_j) == UINT32_MAX) return {(uint32_t)cursor_i, (uint32_t)cursor_j};
        }
        
        cursor_i = 1;
        cursor_j = 1;
        return {0, 0};
    }
    
    void sleep(unsigned short amount) const {
        for (unsigned short i = 0; i < amount && *running_flag; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    std::pair<std::string, std::string> getNames(std::pair<uint32_t, uint32_t> comb) const {
        std::vector<std::string> names = data.getNames({comb.first, comb.second});
        if (names[0] > names[1]) std::swap(names[0], names[1]);
        return {names[0], names[1]};
    }

    void loadHeaders(std::string file) {
        std::ifstream f(file);
        if (f.is_open()) {
            std::string line;
            while (std::getline(f, line)) {
                if (!line.empty() && line.find(':') != std::string::npos) {
                    if (line.back() == '\r') line.pop_back();
                    headers.push_back(line);
                }
            }
            printer.pushLeft("[Config] Loaded " + std::to_string(headers.size()) + " headers from headers.txt");
        } else {
            printer.pushLeft("[Config] Warning: headers.txt not found. Cloudflare will likely block requests.");
        }
    }

    static void logDebugInfo(const std::string& a, const std::string& b, const NetworkClient::Response& res, const std::string& errorMsg) {
        static thread_local std::mt19937 gen(std::random_device{}());
        static thread_local std::uniform_int_distribution<> dis(0, 61);
        static const char alphanum[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

        std::string tmp_s;
        tmp_s.reserve(8);

        for (int i = 0; i < 8; ++i) {
            tmp_s += alphanum[dis(gen)];
        }
        
        std::ofstream log("debug_log_" + tmp_s + ".txt", std::ios::app);
        if (log.is_open()) {
            auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
            log << "--- [" << std::put_time(std::localtime(&now), "%F %T") << "] ---\n";
            log << "Error: " << errorMsg << '\n';
            log << "Pair: " << a << " + " << b << '\n';
            log << "Status: " << res.status_code << '\n';
            log << "Error: " << res.error << '\n';
            log << "Body Snippet: " << res.body.substr(0, 200) << "...\n\n";
        }
    }

    static std::string urlEncode(const std::string &value) {
        std::ostringstream escaped;
        escaped.fill('0');
        escaped << std::hex;
        for (char c : value) {
            if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                escaped << c;
            } else {
                escaped << std::uppercase;
                escaped << '%' << std::setw(2) << int((unsigned char)c);
                escaped << std::nouppercase;
            }
        }
        return escaped.str();
    }
};

#endif