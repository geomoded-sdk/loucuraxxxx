#include "core/log.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <mutex>

namespace lx {

namespace {
std::atomic<int> g_level{static_cast<int>(LogLevel::Info)};
std::mutex g_mutex;

const char* level_tag(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info: return "INFO ";
        case LogLevel::Warn: return "WARN ";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Off: return "OFF  ";
    }
    return "?????";
}
}  // namespace

void set_log_level(LogLevel level) {
    g_level.store(static_cast<int>(level), std::memory_order_relaxed);
}

LogLevel log_level() {
    return static_cast<LogLevel>(g_level.load(std::memory_order_relaxed));
}

void log_message(LogLevel level, const std::string& message) {
    if (static_cast<int>(level) < g_level.load(std::memory_order_relaxed)) return;

    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm);

    std::lock_guard<std::mutex> lock(g_mutex);
    std::fprintf(stderr, "[%s][%s] %s\n", stamp, level_tag(level), message.c_str());
}

}  // namespace lx
