// LoucuraXXXX GPU - runtime loader for OpenGL 3.3 core + EGL.
//
// The GL backend must compile on every target, including machines with no
// OpenGL development headers and machines with no GL at all. It therefore
// declares the small subset of the GL/EGL ABI it needs itself and resolves the
// symbols at runtime, the way a GL loader library does. No system header is
// required and nothing is linked against libGL: if the libraries are absent,
// load() fails and the backend reports BackendUnavailable.
//
// Only the entry points the backend actually calls are declared. A required
// entry point that cannot be resolved is a hard load failure, never a silent
// null call.
#pragma once

#include <string>

namespace lx::backends::gl {

// --- GL ABI types (subset) ---
using GLenum = unsigned int;
using GLboolean = unsigned char;
using GLbitfield = unsigned int;
using GLubyte = unsigned char;
using GLint = int;
using GLuint = unsigned int;
using GLsizei = int;
using GLfloat = float;
using GLchar = char;

// --- EGL ABI types (subset) ---
using EGLDisplay = void*;
using EGLContext = void*;
using EGLSurface = void*;
using EGLConfig = void*;
using EGLint = int;
using EGLBoolean = unsigned int;
using EGLenum = unsigned int;

// --- GL enums (subset) ---
enum : GLenum {
    GL_NO_ERROR = 0,
    GL_TEXTURE_2D = 0x0DE1,
    GL_TEXTURE0 = 0x84C0,
    GL_TEXTURE_MIN_FILTER = 0x2801,
    GL_TEXTURE_MAG_FILTER = 0x2800,
    GL_TEXTURE_WRAP_S = 0x2802,
    GL_TEXTURE_WRAP_T = 0x2803,
    GL_NEAREST = 0x2600,
    GL_LINEAR = 0x2601,
    GL_CLAMP_TO_EDGE = 0x812F,
    GL_RGBA = 0x1908,
    GL_RGBA8 = 0x8058,
    GL_UNSIGNED_BYTE = 0x1401,
    GL_FLOAT = 0x1406,
    GL_FRAMEBUFFER = 0x8D40,
    GL_COLOR_ATTACHMENT0 = 0x8CE0,
    GL_FRAMEBUFFER_COMPLETE = 0x8CD5,
    GL_ARRAY_BUFFER = 0x8892,
    GL_UNIFORM_BUFFER = 0x8A11,
    GL_STATIC_DRAW = 0x88E4,
    GL_DYNAMIC_DRAW = 0x88E8,
    GL_VERTEX_SHADER = 0x8B31,
    GL_FRAGMENT_SHADER = 0x8B30,
    GL_COMPILE_STATUS = 0x8B81,
    GL_LINK_STATUS = 0x8B82,
    GL_INFO_LOG_LENGTH = 0x8B84,
    GL_VERSION = 0x1F02,
    GL_RENDERER = 0x1F01,
    GL_VENDOR = 0x1F00,
    GL_MAX_TEXTURE_SIZE = 0x0D33,
    GL_COLOR_BUFFER_BIT = 0x00004000,
    GL_PACK_ALIGNMENT = 0x0D05,
    GL_UNPACK_ALIGNMENT = 0x0CF5,
    GL_TRIANGLES = 0x0004,
};

// --- EGL enums (subset) ---
enum : EGLint {
    EGL_SUCCESS = 0x3000,
    EGL_NONE = 0x3038,
    EGL_VENDOR = 0x3053,
    EGL_VERSION = 0x3054,
    EGL_EXTENSIONS = 0x3055,
    EGL_ALPHA_SIZE = 0x3021,
    EGL_BLUE_SIZE = 0x3022,
    EGL_GREEN_SIZE = 0x3023,
    EGL_RED_SIZE = 0x3024,
    EGL_SURFACE_TYPE = 0x3033,
    EGL_PBUFFER_BIT = 0x0001,
    EGL_RENDERABLE_TYPE = 0x3040,
    EGL_OPENGL_BIT = 0x0008,
    EGL_CONTEXT_MAJOR_VERSION = 0x3098,
    EGL_CONTEXT_MINOR_VERSION = 0x30FB,
    EGL_CONTEXT_OPENGL_PROFILE_MASK = 0x30FD,
    EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT = 0x00000001,
};

enum : EGLenum {
    EGL_OPENGL_API = 0x30A2,
    EGL_PLATFORM_SURFACELESS_MESA = 0x31DD,
};

// The GL entry points the backend calls. Every member is required: load()
// fails if any of them cannot be resolved.
struct GlApi {
    const GLubyte* (*GetString)(GLenum);
    GLenum (*GetError)();
    void (*GetIntegerv)(GLenum, GLint*);

    void (*GenTextures)(GLsizei, GLuint*);
    void (*DeleteTextures)(GLsizei, const GLuint*);
    void (*BindTexture)(GLenum, GLuint);
    void (*TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum,
                       const void*);
    void (*TexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum,
                          const void*);
    void (*TexParameteri)(GLenum, GLenum, GLint);
    void (*GetTexImage)(GLenum, GLint, GLenum, GLenum, void*);
    void (*ActiveTexture)(GLenum);
    void (*PixelStorei)(GLenum, GLint);

    void (*GenFramebuffers)(GLsizei, GLuint*);
    void (*BindFramebuffer)(GLenum, GLuint);
    void (*FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
    GLenum (*CheckFramebufferStatus)(GLenum);
    void (*DeleteFramebuffers)(GLsizei, const GLuint*);

    void (*GenVertexArrays)(GLsizei, GLuint*);
    void (*BindVertexArray)(GLuint);
    void (*DeleteVertexArrays)(GLsizei, const GLuint*);

    void (*GenBuffers)(GLsizei, GLuint*);
    void (*BindBuffer)(GLenum, GLuint);
    void (*BufferData)(GLenum, long long, const void*, GLenum);
    void (*BufferSubData)(GLenum, long long, long long, const void*);
    void (*DeleteBuffers)(GLsizei, const GLuint*);
    void (*BindBufferBase)(GLenum, GLuint, GLuint);

    GLint (*GetUniformLocation)(GLuint, const GLchar*);
    void (*Uniform1i)(GLint, GLint);

    GLuint (*CreateShader)(GLenum);
    void (*ShaderSource)(GLuint, GLsizei, const GLchar* const*, const GLint*);
    void (*CompileShader)(GLuint);
    void (*GetShaderiv)(GLuint, GLenum, GLint*);
    void (*GetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
    void (*DeleteShader)(GLuint);

    GLuint (*CreateProgram)();
    void (*AttachShader)(GLuint, GLuint);
    void (*LinkProgram)(GLuint);
    void (*GetProgramiv)(GLuint, GLenum, GLint*);
    void (*GetProgramInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
    void (*UseProgram)(GLuint);
    void (*DeleteProgram)(GLuint);

    void (*Viewport)(GLint, GLint, GLsizei, GLsizei);
    void (*Disable)(GLenum);
    void (*ClearColor)(GLfloat, GLfloat, GLfloat, GLfloat);
    void (*Clear)(GLbitfield);
    void (*DrawArrays)(GLenum, GLint, GLsizei);
    void (*Finish)();
    void (*ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);
};

// The EGL entry points the backend calls. GetPlatformDisplayEXT is optional
// and resolved separately; when absent the backend uses eglGetDisplay.
struct EglApi {
    EGLDisplay (*GetDisplay)(void*);
    EGLBoolean (*Initialize)(EGLDisplay, EGLint*, EGLint*);
    EGLBoolean (*Terminate)(EGLDisplay);
    const char* (*QueryString)(EGLDisplay, EGLint);
    EGLint (*GetError)();
    EGLBoolean (*ChooseConfig)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*);
    EGLContext (*CreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint*);
    EGLBoolean (*DestroyContext)(EGLDisplay, EGLContext);
    EGLBoolean (*MakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
    EGLBoolean (*BindAPI)(EGLenum);
    EGLDisplay (*GetPlatformDisplayEXT)(EGLenum, void*, const EGLint*);
    void* (*GetProcAddress)(const char*);
    // Optional. Releases the per-thread EGL state a driver caches on first use;
    // without it a one-time driver allocation stays live at process exit.
    EGLBoolean (*ReleaseThread)();
};

// Loads EGL and GL and resolves every required entry point. On failure returns
// false and fills `error` with the reason (missing library or entry point).
// Safe to call repeatedly; the shared libraries are opened once.
bool load(GlApi& gl, EglApi& egl, std::string& error);

// True once load() has succeeded.
bool loaded();

}  // namespace lx::backends::gl
