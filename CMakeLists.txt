# Alternative: open the folder in Visual Studio ("Open Folder") or build with CMake.
cmake_minimum_required(VERSION 3.20)
project(Skyline CXX RC)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
add_executable(Skyline WIN32
  src/main.cpp src/bar.cpp src/tray.cpp src/status.cpp src/menus.cpp
  src/dock.cpp src/tiling.cpp src/settings.cpp src/icons.cpp src/hotcorners.cpp src/preview.cpp src/applist.cpp src/startmenu.cpp src/nowplaying.cpp src/garden.cpp src/update.cpp src/dialog.cpp src/backup.cpp src/filesearch.cpp src/flyouts.cpp src/look.cpp src/mission.cpp src/systheme.cpp src/writing.cpp src/skyline.rc)
target_compile_definitions(Skyline PRIVATE UNICODE _UNICODE _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A00000C NOMINMAX)
if(MSVC)
  target_compile_options(Skyline PRIVATE /utf-8 /EHsc)
  target_link_options(Skyline PRIVATE /ENTRY:wWinMainCRTStartup /MANIFEST:NO)
else()
  target_link_options(Skyline PRIVATE -municode -static)
endif()
target_link_libraries(Skyline PRIVATE d2d1 dwrite windowscodecs dwmapi shcore shlwapi shell32 ole32 oleaut32
  uuid comctl32 wlanapi iphlpapi version propsys psapi user32 gdi32 advapi32 secur32 powrprof runtimeobject winhttp bcrypt crypt32)
