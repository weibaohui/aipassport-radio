# 网络收音机 应用工程根。
#
#   main/                     应用业务(电台列表、流客户端、MP3 播放、UI)
#   components/framework/     框架 submodule(aipassport-fw),内含 appfw 与 bsp
#
# ESP-IDF 只扫描 components/ 的直接子目录、不递归,因此把整个框架仓作为
# 一个 submodule 挂在 components/framework,再用 EXTRA_COMPONENT_DIRS 把它
# 加入组件搜索路径。修复只落框架仓一处,各应用 submodule update 即得。
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components/framework")

project(FoloToy-AI-Passport)
