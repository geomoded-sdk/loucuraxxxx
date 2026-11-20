// LoucuraXXXX GPU - runtime loader for OpenGL 3.3 core + EGL.
//
// Resolves the GL and EGL entry points the backend needs without linking
// against them, so the build stays free of GL development headers and the
// backend degrades to BackendUnavailable when the libraries are missing.
//
// Platform notes:
//  * Linux/BSD use libEGL.so.1 (EGL) and libGL.so.1 (the GL entry points).
//  * Windows has no EGL of its own; an EGL implementation (ANGLE) may be
//    present as libEGL.dll, and the plain GL 3.3 path uses opengl32.dll, whose
//    only exported GL functions are the 1.1 set. Everything above 1.1 is
//    reached through wglGetProcAddress, so this loader resolves GL symbols from
//    opengl32.dll first and then through that extension entry point.
//  * macOS is deferred for 0.0.2 (see AGENTS.md); this loader is not built
//    there.
//
// The backend only targets 64-bit hosts, where the Win32 __stdcall decoration
// on GL entry points is a no-op, so the plain function-pointer types above are
// correct.
#include "backends/opengl/gl_loader.hpp"

#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace lx::backends::gl {

namespace {

// Minimal platform dynamic-library shim. Kept private so the rest of the file
// reads the same on every OS.
class SharedLibrary {
public:
    SharedLibrary() = default;
    ~SharedLibrary() { close(); }
    SharedLibrary(const SharedLibrary&) = delete;
    SharedLibrary& operator=(const SharedLibrary&) = delete;

    bool open_first(const std::vector<const char*>& names) {
        for (const char* name : names) {
            if (open(name)) return true;
        }
        return false;
    }

    void* symbol(const char* name) const {
#if defined(_WIN32)
        return handle_ ? reinterpret_cast<void*>(::GetProcAddress(handle_, name)) : nullptr;
#else
        return handle_ ? ::dlsym(handle_, name) : nullptr;
#endif
    }

    bool is_open() const { return handle_ != nullptr; }

    void close() {
        if (!handle_) return;
#if defined(_WIN32)
        ::FreeLibrary(handle_);
#else
        ::dlclose(handle_);
#endif
        handle_ = nullptr;
    }

private:
    bool open(const char* name) {
#if defined(_WIN32)
        handle_ = ::LoadLibraryA(name);
#else
        handle_ = ::dlopen(name, RTLD_NOW | RTLD_LOCAL);
#endif
        return handle_ != nullptr;
    }

#if defined(_WIN32)
    HMODULE handle_ = nullptr;
#else
    void* handle_ = nullptr;
#endif
};

std::mutex g_mutex;
bool g_loaded = false;
SharedLibrary g_egl;
SharedLibrary g_gl;
EglApi g_egl_api{};
GlApi g_gl_api{};
#if defined(_WIN32)
// wglGetProcAddress from opengl32.dll, used to reach GL functions above 1.1.
void* (*g_wgl_get_proc_address)(const char*) = nullptr;
#endif

// Copies a resolved symbol into `slot`, recording the name if it is missing.
template <typename Fn>
bool bind(SharedLibrary& lib, const char* name, Fn& slot, std::string& missing) {
    void* sym = lib.symbol(name);
    if (!sym) {
        if (missing.empty()) missing = name;
        return false;
    }
    std::memcpy(&slot, &sym, sizeof(slot));
    return true;
}

// Resolves a GL symbol from the GL library, then (on Windows) through
// wglGetProcAddress, which is the only way to reach GL > 1.1 there.
template <typename Fn>
bool bind_gl(const char* name, Fn& slot, std::string& missing) {
    void* sym = g_gl.symbol(name);
#if defined(_WIN32)
    if (!sym && g_wgl_get_proc_address) sym = g_wgl_get_proc_address(name);
#endif
    if (!sym) {
        if (missing.empty()) missing = name;
        return false;
    }
    std::memcpy(&slot, &sym, sizeof(slot));
    return true;
}

}  // namespace

bool loaded() { return g_loaded; }

bool load(GlApi& gl, EglApi& egl, std::string& error) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_loaded) {
        gl = g_gl_api;
        egl = g_egl_api;
        return true;
    }

    // --- EGL ---
    if (!g_egl.is_open()) {
        const std::vector<const char*> egl_names = {
#if defined(_WIN32)
            "libEGL.dll", "EGL.dll",
#elif defined(__APPLE__)
            "libEGL.dylib",
#else
            "libEGL.so.1", "libEGL.so",
#endif
        };
        if (!g_egl.open_first(egl_names)) {
            error = "no EGL library found (looked for libEGL)";
            return false;
        }
    }

    std::string missing;
    bool ok = true;
    ok &= bind(g_egl, "eglGetDisplay", g_egl_api.GetDisplay, missing);
    ok &= bind(g_egl, "eglInitialize", g_egl_api.Initialize, missing);
    ok &= bind(g_egl, "eglTerminate", g_egl_api.Terminate, missing);
    ok &= bind(g_egl, "eglQueryString", g_egl_api.QueryString, missing);
    ok &= bind(g_egl, "eglGetError", g_egl_api.GetError, missing);
    ok &= bind(g_egl, "eglChooseConfig", g_egl_api.ChooseConfig, missing);
    ok &= bind(g_egl, "eglCreateContext", g_egl_api.CreateContext, missing);
    ok &= bind(g_egl, "eglDestroyContext", g_egl_api.DestroyContext, missing);
    ok &= bind(g_egl, "eglMakeCurrent", g_egl_api.MakeCurrent, missing);
    ok &= bind(g_egl, "eglBindAPI", g_egl_api.BindAPI, missing);
    ok &= bind(g_egl, "eglGetProcAddress", g_egl_api.GetProcAddress, missing);
    // Optional: a driver without it still works, we just cannot release its
    // per-thread state.
    bind(g_egl, "eglReleaseThread", g_egl_api.ReleaseThread, missing);
    if (!ok) {
        error = "EGL is missing a required entry point: " + missing;
        g_egl.close();
        return false;
    }
    // Optional: the surfaceless platform, which is how a headless context is
    // created without a display server. It is an EGL extension, so it is
    // reached through eglGetProcAddress rather than as a library symbol.
    // Absence is not fatal: eglGetDisplay is the fallback.
    g_egl_api.GetPlatformDisplayEXT =
        reinterpret_cast<EGLDisplay (*)(EGLenum, void*, const EGLint*)>(
            g_egl_api.GetProcAddress("eglGetPlatformDisplayEXT"));

    // --- GL ---
    if (!g_gl.is_open()) {
        const std::vector<const char*> gl_names = {
#if defined(_WIN32)
            "opengl32.dll",
#elif defined(__APPLE__)
            "libGL.dylib",
#else
            "libGL.so.1", "libGL.so", "libOpenGL.so.0",
#endif
        };
        if (!g_gl.open_first(gl_names)) {
            error = "no OpenGL library found (looked for libGL)";
            g_egl.close();
            return false;
        }
    }
#if defined(_WIN32)
    g_wgl_get_proc_address = reinterpret_cast<void* (*)(const char*)>(
        g_gl.symbol("wglGetProcAddress"));
#endif

    missing.clear();
    ok = true;
    ok &= bind_gl("glGetString", g_gl_api.GetString, missing);
    ok &= bind_gl("glGetError", g_gl_api.GetError, missing);
    ok &= bind_gl("glGetIntegerv", g_gl_api.GetIntegerv, missing);
    ok &= bind_gl("glGenTextures", g_gl_api.GenTextures, missing);
    ok &= bind_gl("glDeleteTextures", g_gl_api.DeleteTextures, missing);
    ok &= bind_gl("glBindTexture", g_gl_api.BindTexture, missing);
    ok &= bind_gl("glTexImage2D", g_gl_api.TexImage2D, missing);
    ok &= bind_gl("glTexSubImage2D", g_gl_api.TexSubImage2D, missing);
    ok &= bind_gl("glTexParameteri", g_gl_api.TexParameteri, missing);
    ok &= bind_gl("glGetTexImage", g_gl_api.GetTexImage, missing);
    ok &= bind_gl("glActiveTexture", g_gl_api.ActiveTexture, missing);
    ok &= bind_gl("glPixelStorei", g_gl_api.PixelStorei, missing);
    ok &= bind_gl("glGenFramebuffers", g_gl_api.GenFramebuffers, missing);
    ok &= bind_gl("glBindFramebuffer", g_gl_api.BindFramebuffer, missing);
    ok &= bind_gl("glFramebufferTexture2D", g_gl_api.FramebufferTexture2D, missing);
    ok &= bind_gl("glCheckFramebufferStatus", g_gl_api.CheckFramebufferStatus, missing);
    ok &= bind_gl("glDeleteFramebuffers", g_gl_api.DeleteFramebuffers, missing);
    ok &= bind_gl("glGenVertexArrays", g_gl_api.GenVertexArrays, missing);
    ok &= bind_gl("glBindVertexArray", g_gl_api.BindVertexArray, missing);
    ok &= bind_gl("glDeleteVertexArrays", g_gl_api.DeleteVertexArrays, missing);
    ok &= bind_gl("glGenBuffers", g_gl_api.GenBuffers, missing);
    ok &= bind_gl("glBindBuffer", g_gl_api.BindBuffer, missing);
    ok &= bind_gl("glBufferData", g_gl_api.BufferData, missing);
    ok &= bind_gl("glBufferSubData", g_gl_api.BufferSubData, missing);
    ok &= bind_gl("glDeleteBuffers", g_gl_api.DeleteBuffers, missing);
    ok &= bind_gl("glBindBufferBase", g_gl_api.BindBufferBase, missing);
    ok &= bind_gl("glGetUniformLocation", g_gl_api.GetUniformLocation, missing);
    ok &= bind_gl("glUniform1i", g_gl_api.Uniform1i, missing);
    ok &= bind_gl("glCreateShader", g_gl_api.CreateShader, missing);
    ok &= bind_gl("glShaderSource", g_gl_api.ShaderSource, missing);
    ok &= bind_gl("glCompileShader", g_gl_api.CompileShader, missing);
    ok &= bind_gl("glGetShaderiv", g_gl_api.GetShaderiv, missing);
    ok &= bind_gl("glGetShaderInfoLog", g_gl_api.GetShaderInfoLog, missing);
    ok &= bind_gl("glDeleteShader", g_gl_api.DeleteShader, missing);
    ok &= bind_gl("glCreateProgram", g_gl_api.CreateProgram, missing);
    ok &= bind_gl("glAttachShader", g_gl_api.AttachShader, missing);
    ok &= bind_gl("glLinkProgram", g_gl_api.LinkProgram, missing);
    ok &= bind_gl("glGetProgramiv", g_gl_api.GetProgramiv, missing);
    ok &= bind_gl("glGetProgramInfoLog", g_gl_api.GetProgramInfoLog, missing);
    ok &= bind_gl("glUseProgram", g_gl_api.UseProgram, missing);
    ok &= bind_gl("glDeleteProgram", g_gl_api.DeleteProgram, missing);
    ok &= bind_gl("glViewport", g_gl_api.Viewport, missing);
    ok &= bind_gl("glDisable", g_gl_api.Disable, missing);
    ok &= bind_gl("glClearColor", g_gl_api.ClearColor, missing);
    ok &= bind_gl("glClear", g_gl_api.Clear, missing);
    ok &= bind_gl("glDrawArrays", g_gl_api.DrawArrays, missing);
    ok &= bind_gl("glFinish", g_gl_api.Finish, missing);
    ok &= bind_gl("glReadPixels", g_gl_api.ReadPixels, missing);
    if (!ok) {
        error = "OpenGL is missing a required entry point: " + missing;
        g_gl.close();
        g_egl.close();
        return false;
    }

    gl = g_gl_api;
    egl = g_egl_api;
    g_loaded = true;
    return true;
}

}  // namespace lx::backends::gl
