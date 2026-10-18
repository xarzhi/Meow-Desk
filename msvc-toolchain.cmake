# MSVC toolchain — 手动固化 INCLUDE/LIB 路径（绕过 vcvars64 的 reg.exe）
set(MSVC_ROOT "C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.50.35717")
set(SDK_ROOT  "D:/Windows Kits/10")
set(SDK_VER   "10.0.22621.0")

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x64)
set(CMAKE_SYSTEM_VERSION "${SDK_VER}")

set(CMAKE_C_COMPILER   "${MSVC_ROOT}/bin/Hostx64/x64/cl.exe")
set(CMAKE_CXX_COMPILER "${MSVC_ROOT}/bin/Hostx64/x64/cl.exe")

set(_inc
    "${MSVC_ROOT}/include"
    "${MSVC_ROOT}/atlmfc/include"
    "${SDK_ROOT}/Include/${SDK_VER}/ucrt"
    "${SDK_ROOT}/Include/${SDK_VER}/um"
    "${SDK_ROOT}/Include/${SDK_VER}/shared"
    "${SDK_ROOT}/Include/${SDK_VER}/winrt"
    "${SDK_ROOT}/Include/${SDK_VER}/cppwinrt"
)
set(_lib
    "${MSVC_ROOT}/lib/x64"
    "${MSVC_ROOT}/atlmfc/lib/x64"
    "${SDK_ROOT}/Lib/${SDK_VER}/ucrt/x64"
    "${SDK_ROOT}/Lib/${SDK_VER}/um/x64"
)

string(JOIN ";" _inc_flags ${_inc})
string(JOIN ";" _lib_flags ${_lib})

# 通过环境变量传给 cl/link（Ninja 下最可靠）
set(ENV{INCLUDE} "${_inc_flags}")
set(ENV{LIB}     "${_lib_flags}")

# 同时作为编译器/链接器 flags 兜底
foreach(d ${_inc})
    string(APPEND CMAKE_CXX_FLAGS " /I\"${d}\"")
endforeach()
foreach(l ${_lib})
    string(APPEND CMAKE_EXE_LINKER_FLAGS " /LIBPATH:\"${l}\"")
endforeach()
