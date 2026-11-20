// LoucuraXXXX GPU - minimal thread-safe logging.
#pragma once

#include <sstream>
#include <string>

namespace lx {

enum class LogLevel { Trace = 0, Debug, Info, Warn, Error, Off };

// Sets the global minimum level. Defaults to Info. Thread-safe.
void set_log_level(LogLevel level);
LogLevel log_level();

// Writes a formatted line to stderr with a timestamp and level tag.
// Thread-safe: a single line is emitted atomically.
void log_message(LogLevel level, const std::string& message);

namespace detail {
class LogStream {
public:
    explicit LogStream(LogLevel level) : level_(level) {}
    ~LogStream() { log_message(level_, buffer_.str()); }
    template <typename T>
    LogStream& operator<<(const T& value) {
        buffer_ << value;
        return *this;
    }

private:
    LogLevel level_;
    std::ostringstream buffer_;
};
}  // namespace detail

}  // namespace lx

#define LX_LOG(level)                                     \
    if (static_cast<int>(level) <                           \
        static_cast<int>(::lx::log_level())) {        \
    } else                                                  \
        ::lx::detail::LogStream(level)

#define LX_TRACE LX_LOG(::lx::LogLevel::Trace)
#define LX_DEBUG LX_LOG(::lx::LogLevel::Debug)
#define LX_INFO LX_LOG(::lx::LogLevel::Info)
#define LX_WARN LX_LOG(::lx::LogLevel::Warn)
#define LX_ERROR LX_LOG(::lx::LogLevel::Error)
