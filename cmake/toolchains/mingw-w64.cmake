# Cross-compile for 64-bit Windows with mingw-w64 from a Linux host.
#
#   cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64.cmake
#
# The posix thread model is required: the project uses std::thread, which the
# win32 thread model does not provide. Adjust the compiler names below if your
# distribution uses a different triplet prefix.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc-posix)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++-posix)

set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# Link the C++ runtime statically so the produced .exe files run on a plain
# Windows install (and under Wine) without shipping libstdc++-6.dll and
# libgcc_s_seh-1.dll alongside them.
set(CMAKE_EXE_LINKER_FLAGS_INIT "-static -static-libgcc -static-libstdc++")
