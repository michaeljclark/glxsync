cmake_minimum_required(VERSION 3.10)

project(app_x11)

set(CMAKE_C_STANDARD 11)

find_package(Threads REQUIRED)

add_executable(gl2_xsync src/gl2_xsync.c)
target_link_libraries(gl2_xsync Threads::Threads X11 Xext GLX GL m)
