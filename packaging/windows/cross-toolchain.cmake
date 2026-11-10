# Cross-compiling the Windows build from Linux with the host clang and an MSYS2 CLANG64 sysroot
# (packaging/windows/cross-build.sh fetches it). Only that script uses this file; the Linux and
# the native MSYS2 builds do not.
#
#   cmake -S gpu -B out/win/build -DCMAKE_TOOLCHAIN_FILE=packaging/windows/cross-toolchain.cmake \
#         -DSANCTUARY_WIN_ROOT=out/win/root
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

if (NOT SANCTUARY_WIN_ROOT)
    set(SANCTUARY_WIN_ROOT "$ENV{SANCTUARY_WIN_ROOT}")
endif ()
if (NOT SANCTUARY_WIN_ROOT)
    message(FATAL_ERROR "Set SANCTUARY_WIN_ROOT to the folder that holds clang64/")
endif ()
get_filename_component(SANCTUARY_WIN_ROOT "${SANCTUARY_WIN_ROOT}" ABSOLUTE)
# try_compile projects get the toolchain file again: pass the root on.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES SANCTUARY_WIN_ROOT)
set(ENV{SANCTUARY_WIN_ROOT} "${SANCTUARY_WIN_ROOT}")

set(sysroot "${SANCTUARY_WIN_ROOT}/clang64")
set(triple x86_64-w64-windows-gnu)
file(GLOB resource_dirs "${sysroot}/lib/clang/*")
list(GET resource_dirs 0 resource_dir)

set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_COMPILER_TARGET ${triple})
set(CMAKE_CXX_COMPILER_TARGET ${triple})
set(CMAKE_RC_COMPILER llvm-windres)
set(CMAKE_AR llvm-ar)
set(CMAKE_RANLIB llvm-ranlib)
set(CMAKE_SYSROOT "${sysroot}")

# The host clang's own headers (stddef.h, x86intrin.h...); MSYS2's compiler-rt package only has
# the libraries, so linking takes its resource folder.
# Source paths in the binaries (assert messages, __FILE__) relative to the builder's home.
set(CMAKE_C_FLAGS_INIT "-ffile-prefix-map=$ENV{HOME}=~")
set(CMAKE_CXX_FLAGS_INIT "-stdlib=libc++ -ffile-prefix-map=$ENV{HOME}=~")
set(link "-fuse-ld=lld -rtlib=compiler-rt -unwindlib=libunwind -resource-dir=${resource_dir}")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${link}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${link}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${link}")

# Libraries and headers from the sysroot only; programs (glslang, pkg-config) from the host.
set(CMAKE_FIND_ROOT_PATH "${sysroot}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# The .pc files say prefix=/clang64: the sysroot is the folder above it.
set(ENV{PKG_CONFIG_LIBDIR} "${sysroot}/lib/pkgconfig:${sysroot}/share/pkgconfig")
set(ENV{PKG_CONFIG_PATH} "")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "${SANCTUARY_WIN_ROOT}")
