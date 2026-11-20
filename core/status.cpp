#include "core/status.hpp"

namespace lx {

const char* to_string(ErrorCode code) {
    switch (code) {
        case ErrorCode::Ok: return "Ok";
        case ErrorCode::Unknown: return "Unknown";
        case ErrorCode::InvalidArgument: return "InvalidArgument";
        case ErrorCode::OutOfRange: return "OutOfRange";
        case ErrorCode::NotSupported: return "NotSupported";
        case ErrorCode::NotImplemented: return "NotImplemented";
        case ErrorCode::OutOfMemory: return "OutOfMemory";
        case ErrorCode::IoError: return "IoError";
        case ErrorCode::ParseError: return "ParseError";
        case ErrorCode::DeviceLost: return "DeviceLost";
        case ErrorCode::ResourceBusy: return "ResourceBusy";
        case ErrorCode::Timeout: return "Timeout";
        case ErrorCode::Cancelled: return "Cancelled";
        case ErrorCode::InitializationFailed: return "InitializationFailed";
        case ErrorCode::BackendUnavailable: return "BackendUnavailable";
    }
    return "Unknown";
}

std::string Status::to_string() const {
    if (ok()) return "Ok";
    return std::string(lx::to_string(code_)) + ": " + message_;
}

}  // namespace lx
