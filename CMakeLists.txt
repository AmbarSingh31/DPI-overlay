cmake_minimum_required(VERSION 3.20)
project(DPIOverlay CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (MSVC)
  add_definitions(-DUNICODE -D_UNICODE)
  add_compile_options(/W4 /permissive-)
endif()

add_executable(DPIOverlay
  src/main.cpp
  resources/overlay.rc
)

target_include_directories(DPIOverlay PRIVATE src)

# Link required Windows libraries
if (MSVC)
  target_link_libraries(DPIOverlay PRIVATE
    Gdiplus
    Shcore
    User32
    Gdi32
    Dwmapi
  )
endif()

set_target_properties(DPIOverlay PROPERTIES
  WIN32_EXECUTABLE YES
  OUTPUT_NAME "DPIOverlay"
)


