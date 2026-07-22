cmake_minimum_required(VERSION 3.16)
set(ENV{BTSTACK_ROOT} "${CMAKE_CURRENT_SOURCE_DIR}/components/btstack")
set(CONFIG_BTSTACK_MESH_ENABLED OFF CACHE BOOL "" FORCE)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(vetera-bridge)
