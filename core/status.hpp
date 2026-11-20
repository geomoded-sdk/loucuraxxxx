// LoucuraXXXX GPU - error handling primitives.
//
// The project never throws across module boundaries: every fallible operation
// returns a Status or a Result<T>. This keeps the hot paths (render, filter,
// scheduling) allocation-light and predictable.
#pragma once

#include <string>
#include <utility>
#include <variant>

namespace lx {

enum class ErrorCode {
    Ok = 0,
    Unknown,
    InvalidArgument,
    OutOfRange,
    NotSupported,
    NotImplemented,
    OutOfMemory,
    IoError,
    ParseError,
    DeviceLost,
    ResourceBusy,
    Timeout,
    Cancelled,
    InitializationFailed,
    BackendUnavailable,
};

const char* to_string(ErrorCode code);

// A Status is either Ok or an error with a code and a human readable message.
class Status {
public:
    Status() = default;
    Status(ErrorCode code, std::string message)
        : code_(code), message_(std::move(message)) {}

    static Status success() { return Status{}; }
    static Status error(ErrorCode code, std::string message) {
        return Status{code, std::move(message)};
    }

    bool ok() const { return code_ == ErrorCode::Ok; }
    explicit operator bool() const { return code_ == ErrorCode::Ok; }
    ErrorCode code() const { return code_; }
    const std::string& message() const { return message_; }

    std::string to_string() const;

private:
    ErrorCode code_ = ErrorCode::Ok;
    std::string message_;
};

// Result carries either a value or an error Status. Never both.
template <typename T>
class Result {
public:
    Result(T value) : data_(std::move(value)) {}
    Result(Status status) : data_(std::move(status)) {}

    bool ok() const { return std::holds_alternative<T>(data_); }
    explicit operator bool() const { return ok(); }

    T& value() { return std::get<T>(data_); }
    const T& value() const { return std::get<T>(data_); }

    Status status() const {
        return ok() ? Status::success() : std::get<Status>(data_);
    }

private:
    std::variant<T, Status> data_;
};

// Helper macro for early return on failure, used throughout the codebase.
#define LX_TRY(expr)                                    \
    do {                                                \
        ::lx::Status lx_status_ = (expr);         \
        if (!lx_status_.ok()) return lx_status_;        \
    } while (false)

#define LX_TRY_ASSIGN(var, expr)                        \
    auto lx_res_##var = (expr);                         \
    if (!lx_res_##var.ok()) return lx_res_##var.status(); \
    auto& var = lx_res_##var.value()

}  // namespace lx
