# Cross-compile toolchain for building the Windows burnout_recomp.exe on Linux
# via mingw-w64. Not part of xboxrecomp's own templates -- the toolkit is
# written MSVC-first; this file exists because building and testing happen on
# this Linux machine via mingw + Wine/DXVK rather than a separate Windows box.
# Same arrangement as the Breakdown control, which is where this came from.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER   x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER  x86_64-w64-mingw32-windres)

set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
