#include "strata/status.h"

namespace strata {

std::string Status::to_string() const {
    const char* label = "unknown";
    switch (code_) {
    case Code::kOk:
        return "OK";
    case Code::kNotFound:
        label = "NotFound";
        break;
    case Code::kCorruption:
        label = "Corruption";
        break;
    case Code::kIoError:
        label = "IOError";
        break;
    case Code::kInvalidArgument:
        label = "InvalidArgument";
        break;
    case Code::kBusy:
        label = "Busy";
        break;
    }
    std::string out(label);
    if (!msg_.empty()) {
        out += ": ";
        out += msg_;
    }
    return out;
}

} // namespace strata
