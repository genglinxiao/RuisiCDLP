#pragma once

#include "usb_dlp/policy.h"

#include <string>

namespace usb_dlp {

bool decode_compiled_policy(
    const std::string& json,
    CompiledPolicy* policy,
    std::string* error);

}  // namespace usb_dlp
