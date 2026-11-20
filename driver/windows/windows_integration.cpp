// LoucuraXXXX GPU - Windows integration.
//
// Windows graphics drivers are WDDM kernel-mode drivers (a .sys plus a user-mode
// driver). Installing one requires signing, administrator rights and a test
// signing policy; a normal process cannot do it and this project will not
// pretend otherwise. The integration therefore reports UserSpaceOnly: it
// detects the WDDM adapter set and the presence of the DXGI/D3D12 runtime, and
// leaves adapter creation to the display-driver model.
//
// Marked experimental: written against documented Win32/DXGI APIs but not
// exercised on a Windows host in this repository's CI.
#include "driver/platform_integration.hpp"

#if defined(_WIN32)

#include <windows.h>
#include <dxgi.h>

#include "core/log.hpp"

namespace lx::driver {

namespace {

bool d3d12_runtime_present() {
    HMODULE module = ::LoadLibraryW(L"d3d12.dll");
    if (module == nullptr) return false;
    ::FreeLibrary(module);
    return true;
}

u32 count_wddm_adapters() {
    IDXGIFactory* factory = nullptr;
    if (FAILED(::CreateDXGIFactory(__uuidof(IDXGIFactory),
                                   reinterpret_cast<void**>(&factory))) ||
        factory == nullptr) {
        return 0;
    }
    u32 count = 0;
    for (UINT i = 0;; ++i) {
        IDXGIAdapter* adapter = nullptr;
        if (factory->EnumAdapters(i, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        if (adapter == nullptr) break;
        ++count;
        adapter->Release();
    }
    factory->Release();
    return count;
}

}  // namespace

class WindowsIntegration : public PlatformIntegration {
public:
    const char* name() const override { return "windows"; }

    IntegrationStatus probe() const override {
        IntegrationStatus status;
        status.platform = "windows";
        status.api = "WDDM / DXGI";
        status.level = IntegrationLevel::UserSpaceOnly;
        // The D3D12 runtime being present is not the same as this layer sharing
        // surfaces through it; no shared-texture path is implemented, so the flag
        // stays false.

        const u32 adapters = count_wddm_adapters();
        status.notes.emplace_back("WDDM adapters detected: " + std::to_string(adapters));
        status.notes.emplace_back(d3d12_runtime_present() ? "Direct3D 12 runtime present"
                                                          : "Direct3D 12 runtime not found");
        status.notes.emplace_back(
            "zero-copy surface sharing is not implemented; frames are copied");
        status.can_expose_adapter = false;
        status.notes.emplace_back(
            "registering a virtual display adapter requires a signed WDDM "
            "kernel-mode driver and administrator rights; out of scope here");
        return status;
    }

    Status connect(const hardware::HardwareInfo&) override {
        status_ = probe();
        connected_ = true;
        LX_INFO << "Windows integration connected (user-space, DXGI)";
        return Status::success();
    }

    void disconnect() override { connected_ = false; }

private:
    IntegrationStatus status_{};
    bool connected_ = false;
};

std::unique_ptr<PlatformIntegration> create_platform_integration() {
    return std::unique_ptr<PlatformIntegration>(new WindowsIntegration());
}

}  // namespace lx::driver

#endif  // _WIN32
