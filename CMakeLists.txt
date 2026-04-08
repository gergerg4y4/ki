cmake_minimum_required(VERSION 3.22)
project(stuffmods)

add_library(stuffmods SHARED stuffmods.cpp)

target_link_libraries(stuffmods android log dl)

target_compile_options(stuffmods PRIVATE -O2 -fvisibility=hidden)

set_target_properties(stuffmods PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
