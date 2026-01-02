#ifndef NETWORK_CLIENT_HPP
#define NETWORK_CLIENT_HPP

#include <string>
#include <vector>
#include <future>
#include <mutex>
#include <stack>
#include <fstream>
#include <curl/curl.h>

class NetworkClient {
public:
    struct Response {
        long status_code;
        std::string body;
        std::string error;
        bool success;
    };

    NetworkClient() {
        std::call_once(initFlag, []() {
            curl_global_init(CURL_GLOBAL_ALL);
            std::atexit([]() {
                curl_global_cleanup();
            });
        });
    }

    ~NetworkClient() {
        std::lock_guard<std::mutex> lock(pool_mutex);
        while (!connection_pool.empty()) {
            curl_easy_cleanup(connection_pool.top());
            connection_pool.pop();
        }
    }

    Response request(const std::string& url, const std::string& method, const std::string& postData = "", const std::vector<std::string>* customHeaders = nullptr, const long timeout = 10L, const std::string& proxy = "") {
        CURL* curl = acquireHandle();
        Response response = {0, "", "", false};

        if (!curl) {
            response.error = "Failed to initialize CURL";
            return response;
        }

        std::string response_string;
        struct curl_slist* headerList = NULL;

        if (customHeaders) {
            for (const auto& h : *customHeaders)
                headerList = curl_slist_append(headerList, h.c_str());
        } else
            for (const auto& h : headers)
                headerList = curl_slist_append(headerList, h.c_str());

        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_string);
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str());
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

        if (!proxy.empty()) {
            curl_easy_setopt(curl, CURLOPT_PROXY, proxy.c_str());
        }

        if (method == "POST" || method == "PUT") {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, postData.c_str());
        }

        curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);

        CURLcode res = curl_easy_perform(curl);

        if (res != CURLE_OK) {
            response.error = curl_easy_strerror(res);
            response.success = false;
        } else {
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status_code);
            response.body = response_string;
            response.success = true;
        }

        if (headerList) curl_slist_free_all(headerList);

        releaseHandle(curl);

        return response;
    }

    std::future<Response> requestAsync(const std::string& url, const std::string& method, const std::string& postData = "", const std::vector<std::string>* headers = nullptr, const long timeout = 10L, const std::string& proxy = "") {
        return std::async(std::launch::async, [=]() {
            return this->request(url, method, postData, headers, timeout, proxy);
        });
    }
    
    size_t loadHeaders(std::string file) {
        std::ifstream f(file);
        if (f.is_open()) {
            std::string line;
            while (std::getline(f, line)) {
                if (!line.empty() && line.find(':') != std::string::npos) {
                    if (line.back() == '\r') line.pop_back();
                    headers.push_back(line);
                }
            }
            return headers.size();
        } else return 0;
    }

    void resizePool(size_t newSize) {
        std::lock_guard<std::mutex> lock(pool_mutex);

        while (connection_pool.size() > newSize) {
            curl_easy_cleanup(connection_pool.top());
            connection_pool.pop();
        }

        while (connection_pool.size() < newSize) {
            CURL* curl = curl_easy_init();
            if (curl) connection_pool.push(curl);
            else break;
        }
    }

private:
    static std::once_flag initFlag;

    std::vector<std::string> headers;

    std::mutex pool_mutex;
        std::stack<CURL*> connection_pool;

    CURL* acquireHandle() {
        std::lock_guard<std::mutex> lock(pool_mutex);
        if (connection_pool.empty()) return curl_easy_init();
        CURL* curl = connection_pool.top();
        connection_pool.pop();
        return curl;
    }

    void releaseHandle(CURL* curl) {
        if (!curl) return;

        curl_easy_reset(curl);

        std::lock_guard<std::mutex> lock(pool_mutex);
        connection_pool.push(curl);
    }

    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
        size_t totalSize = size * nmemb;
        ((std::string*)userp)->append((char*)contents, totalSize);
        return totalSize;
    }
};

std::once_flag NetworkClient::initFlag;

#endif // NETWORK_CLIENT_HPP