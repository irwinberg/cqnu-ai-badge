cmake_minimum_required(VERSION 3.16)
# 本机私密配置（同步服务地址/密钥），不入库
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.secrets")
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.secrets")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(FoloToy-AI-Passport)
