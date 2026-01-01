#ifndef PRINTER_HPP
#define PRINTER_HPP

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// For console size
#ifdef _WIN32
#include <windows.h>
#include <conio.h>
#else
#include <sys/ioctl.h>
#include <unistd.h>
#include <termios.h>
#endif

class Printer {
public:
    Printer() : running(true), changed(false), inputting(false) {
        stats = { 0.0, 0, 0, 0, 0 };

        enableAnsiSupport();
        updateDimensions();
        
        std::cout << "\033[?25l\033[?7l\033[2J\033[3J\033[1;1H" << std::flush;
        
        render_thread = std::thread(&Printer::renderLoop, this);
    }

    ~Printer() {
        {
            std::lock_guard<std::mutex> lock(render_mutex);

            running = false;
            changed = true;
        }
        cv.notify_one();

        if (render_thread.joinable()) {
            render_thread.join();
        }

        std::cout << "\033[?7h\033[?25h";
        std::cout << "\033[" << height << ";1H\n";
    }

    void setStats(double speed, long long elapsed, size_t proxy, size_t total, size_t requests) {
        {
            std::lock_guard<std::mutex> lock(render_mutex);

            stats.speed = speed;
            stats.elapsed = elapsed;
            stats.proxy = proxy;
            stats.total = total;
            stats.requests = requests;
            changed = true;
        }
        cv.notify_one();
    }

    void pushLeft(const std::string& text) {
        {
            std::lock_guard<std::mutex> lock(render_mutex);
            
            if (left_buffer.size() > 100) left_buffer.pop_front();
            left_buffer.push_back(text);
            changed = true;
        }
        cv.notify_one();
    }

    void pushRight(const std::string& text) {
        {
            std::lock_guard<std::mutex> lock(render_mutex);

            if (right_buffer.size() > 100) right_buffer.pop_front();
            right_buffer.push_back(text);
            changed = true;
        }
        cv.notify_one();
    }

    std::string prompt(const std::string& message) {
        std::lock_guard<std::mutex> lock(prompt_mutex);

        std::string input;
        int local_height;
        
        {
            std::unique_lock<std::mutex> r_lock(render_mutex);
            
            inputting = true;
            current_prompt = message;
            changed = true;
            
            updateDimensions(); 
            local_height = height;

            {
                std::lock_guard<std::mutex> io_lock(console_mutex);
                std::cout << "\033[" << local_height << ";1H\033[2K"; 
                std::cout << "\033[0m" << message << std::flush;
                std::cout << "\033[?25h";
            }
        }

        cv.notify_one();

        std::getline(std::cin, input);

        if (running) {
            {
                std::lock_guard<std::mutex> lock(render_mutex);
                
                inputting = false;
                changed = true;
                current_prompt.clear();
            }
            cv.notify_one();

            {
                std::lock_guard<std::mutex> io_lock(console_mutex);
                std::cout << "\033[?25l";
            }
        }
        
        return input;
    }

private:
    std::deque<std::string> left_buffer;
    std::deque<std::string> right_buffer;
    std::string current_prompt;
    
    struct Stats {
        double speed;
        long long elapsed;
        size_t proxy;
        size_t total;
        size_t requests;
    } stats;
    
    int width, height;

    std::atomic<bool> running;
    std::thread render_thread;

    std::mutex prompt_mutex;
    std::mutex console_mutex;
    std::mutex render_mutex;
        std::condition_variable cv;
        bool changed;
    
    std::atomic<bool> inputting;
    
    void enableAnsiSupport() {
#ifdef _WIN32
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD dwMode = 0;
        if (GetConsoleMode(hOut, &dwMode)) {
            dwMode |= 0x0004;
            SetConsoleMode(hOut, dwMode);
        }
        SetConsoleOutputCP(CP_UTF8);
#endif
    }

    void updateDimensions() {
#ifdef _WIN32
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        int columns, rows;
        GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi);
        width = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        height = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
#else
        struct winsize w;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) != -1) {
            width = w.ws_col;
            height = w.ws_row;
        }
#endif
    }

    static std::vector<std::string> wrapText(const std::string& text, int max_width) {
        std::vector<std::string> lines;
        if (max_width < 1) return {text};

        size_t start = 0;
        while (start < text.length()) {
            bool is_first = (start == 0);
            size_t len = is_first ? max_width : max_width - 1;

            if (text.length() - start <= len) {
                std::string segment = text.substr(start);
                lines.push_back(is_first ? segment : " " + segment);
                break;
            }

            size_t space_pos = text.rfind(' ', start + len);
            std::string segment;
            
            if (space_pos != std::string::npos && space_pos > start) {
                size_t split_len = space_pos - start;
                segment = text.substr(start, split_len);
                start += split_len + 1;
            } else {
                segment = text.substr(start, len);
                start += len;
            }
            lines.push_back(is_first ? segment : " " + segment);
        }
        return lines;
    }

    static std::vector<std::string> wrapStatusBar(const std::string& text, int width) {
        std::vector<std::string> lines;
        if (text.empty()) return lines;
        
        int target_width = (width > 2) ? width - 2 : 1;

        std::string delimiter = " | ";
        size_t start = 0;
        size_t end = text.find(delimiter);
        
        std::string current_line;

        auto add_part = [&](const std::string& part) {
            if (current_line.empty()) {
                current_line = part;
            } else {
                if (current_line.length() + delimiter.length() + part.length() > (size_t)target_width) {
                    lines.push_back(" " + current_line); // Add left padding
                    current_line = part;
                } else {
                    current_line += delimiter + part;
                }
            }
        };

        while (end != std::string::npos) {
            add_part(text.substr(start, end - start));
            start = end + delimiter.length();
            end = text.find(delimiter, start);
        }
        add_part(text.substr(start));
        
        if (!current_line.empty()) {
            lines.push_back(" " + current_line);
        }
        
        return lines;
    }

    static std::string padOrTruncate(const std::string& text, int target_len) {
        if ((int)text.length() > target_len) {
            return text.substr(0, target_len);
        }
        return text + std::string(target_len - text.length(), ' ');
    }

    void renderLoop() {
        int last_w = 0, last_h = 0;

        {
            std::lock_guard<std::mutex> lock(render_mutex);
            last_w = width;
            last_h = height;
        }

        while (true) {
            std::vector<std::string> snap_left;
            std::vector<std::string> snap_right;
            std::string snap_prompt;
            Stats snap_stats;
            bool snap_inputting;
            bool dim_changed = false;

            {
                std::unique_lock<std::mutex> lock(render_mutex);
                cv.wait_for(lock, std::chrono::milliseconds(50), [this]{ return changed || !running; });
                
                if (!running) return;

                updateDimensions();
                if (width != last_w || height != last_h) {
                    dim_changed = true;
                    last_w = width;
                    last_h = height;
                    changed = true;
                }

                if (!changed) continue;

                snap_left = std::vector<std::string>(left_buffer.begin(), left_buffer.end());
                snap_right = std::vector<std::string>(right_buffer.begin(), right_buffer.end());
                snap_stats = stats;
                snap_inputting = inputting;
                snap_prompt = current_prompt;
                
                changed = false;
            } 

            // --- Layout Calculation ---
            std::vector<std::string> status_lines_vec;
            int reserved_bottom = 1;

            if (!snap_inputting) {
                std::stringstream status_ss;
                int h = (int)(snap_stats.elapsed / 3600);
                int m = (int)((snap_stats.elapsed % 3600) / 60);
                int s = (int)(snap_stats.elapsed % 60);
                
                char time_buf[16];
                snprintf(time_buf, sizeof(time_buf), "%02d:%02d:%02d", h, m, s);

                status_ss << "Speed: " << std::fixed << std::setprecision(1) << snap_stats.speed << "/s"
                        << " | Time: " << time_buf
                        << " | Total: " << snap_stats.total
                        << " | Proxy: " << snap_stats.proxy
                        << " | Active Requests: " << snap_stats.requests;

                status_lines_vec = wrapStatusBar(status_ss.str(), last_w);
                reserved_bottom = std::max(1, (int)status_lines_vec.size());
            }

            int viewport_height = std::max(0, last_h - reserved_bottom);
            int text_width = (last_w / 2) - 2;

            auto getVisualLines = [&](const std::vector<std::string>& raw, int w, int max_h) {
                std::deque<std::string> visual;
                for (auto it = raw.rbegin(); it != raw.rend(); ++it) {
                    if ((int)visual.size() >= max_h) break;
                    
                    std::vector<std::string> parts = wrapText(*it, w);
                    for (auto bit = parts.rbegin(); bit != parts.rend(); ++bit) {
                        visual.push_front(*bit);
                        if ((int)visual.size() >= max_h) break; 
                    }
                }
                return std::vector<std::string>(visual.begin(), visual.end());
            };

            std::vector<std::string> visual_left = getVisualLines(snap_left, text_width, viewport_height);
            std::vector<std::string> visual_right = getVisualLines(snap_right, text_width, viewport_height);

            std::stringstream ss;

            // 1. Handle Resize
            if (dim_changed) {
                ss << "\033[?7l\033[2J\033[3J";
            }
            
            // 2. Save Cursor (DEC) if inputting
            if (snap_inputting) ss << "\0337";

            ss << "\033[1;1H";

            // 3. Render Viewport
            for (int row = 0; row < viewport_height; ++row) {
                int offset = (int)visual_left.size() - viewport_height + row;
                std::string l = (offset >= 0) ? visual_left[offset] : "";

                offset = (int)visual_right.size() - viewport_height + row;
                std::string r = (offset >= 0) ? visual_right[offset] : "";

                ss << padOrTruncate(l, text_width) << " | " << padOrTruncate(r, text_width) << "\033[K";
                
                if (row < viewport_height - 1) ss << "\n";
            }

            // 4. Render Bottom
            if (!snap_inputting) {
                // Clear leftover lines
                for (int r = viewport_height + 1; r <= last_h; ++r) {
                    ss << "\033[" << r << ";1H\033[2K";
                }
                // Draw Status Bar
                for (size_t i = 0; i < status_lines_vec.size(); ++i) {
                    ss << "\033[" << (viewport_height + 1 + i) << ";1H"; 
                    ss << "\033[7m" << padOrTruncate(status_lines_vec[i], last_w) << "\033[0m";
                }
            } else {
                // Input Mode
                if (dim_changed) {
                    // Reprint prompt on resize
                    ss << "\033[" << last_h << ";1H" << "\033[0m" << snap_prompt;
                } else {
                    // Restore cursor to user position
                    ss << "\0338";
                }
            }

            // --- OUTPUT PHASE ---
            {
                std::lock_guard<std::mutex> io_lock(console_mutex);

                // RACE CONDITION FIX:
                // If the 'inputting' state changed while we were generating this frame,
                // our frame is stale (e.g., it contains a status bar, but prompt() just printed a prompt).
                // We MUST ABORT printing this frame to avoid overwriting the prompt.
                if (inputting != snap_inputting) {
                    continue; 
                }

                std::cout << ss.str() << std::flush;
            }
        }
    }
};

#endif // PRINTER_HPP