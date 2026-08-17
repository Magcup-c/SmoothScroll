cmake_minimum_required(VERSION 3.20)
project(SmoothScrollLib CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# スムーススクロールDLL本体
add_library(SmoothScroll SHARED
    SmoothScroll/SmoothScroll.cpp
    SmoothScroll/PhysicsEngine.cpp
    SmoothScroll/SmoothScroll.h
    SmoothScroll/PhysicsEngine.h
)
target_compile_definitions(SmoothScroll PRIVATE SMOOTHSCROLL_EXPORTS WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_link_libraries(SmoothScroll PRIVATE winmm comctl32)

# デモアプリ
add_executable(SSDemo WIN32 demo/main.cpp)
target_link_libraries(SSDemo PRIVATE SmoothScroll)
target_compile_definitions(SSDemo PRIVATE UNICODE _UNICODE)
