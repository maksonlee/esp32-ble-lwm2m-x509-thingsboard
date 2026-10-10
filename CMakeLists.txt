cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED PROJECT_VER)
    set(PROJECT_VER "v02")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(wifi_prov_mgr)
