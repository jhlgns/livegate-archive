project(livegate)
cmake_minimum_required(VERSION 3.18)

if(NOT CMAKE_INSTALL_PREFIX)
    set(CMAKE_INSTALL_PREFIX "~/.local")
endif()

add_subdirectory(wsServer)


find_package(Threads REQUIRED)

add_executable(livegate server.cpp)
target_include_directories(livegate PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/wsServer/include)
target_link_libraries(livegate ws Threads::Threads)
target_compile_features(livegate PUBLIC cxx_std_11)

install(TARGETS livegate)

