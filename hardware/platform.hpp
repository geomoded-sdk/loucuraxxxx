// LoucuraXXXX GPU - internal platform hooks for hardware detection.
#pragma once

#include "hardware/hardware_info.hpp"

namespace lx::hardware::platform {

// Each function fills the platform-specific portion of the description.
// Implementations live in platform_<os>.cpp and are compiled conditionally.
void detect_cpu(CpuInfo& out);
void detect_memory(MemoryInfo& out);
void detect_os(OperatingSystemInfo& out);
std::vector<GpuDeviceInfo> detect_system_gpus();
u32 logical_processor_count();

}  // namespace lx::hardware::platform
