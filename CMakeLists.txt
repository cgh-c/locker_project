cmake_minimum_required(VERSION 3.10)
project(LockerAgent C)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -g")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# 指定交叉编译工具链
set(CMAKE_C_COMPILER arm-buildroot-linux-gnueabihf-gcc)

# 设置交叉编译环境的根目录
set(SYSROOT $ENV{HOME}/paho-imx6ull-rootfs)

# 头文件路径
include_directories(
    include                          # 项目自身的头文件
    ${SYSROOT}/usr/include           # Paho MQTT 和 cJSON 的头文件（如果有）
    ${CMAKE_SOURCE_DIR}/src          # 让 .c 文件能找到同级头文件（如有需要）
)

# 源文件（包括 cJSON.c）
file(GLOB_RECURSE SOURCES "src/*.c")

# 链接库目录
link_directories(${SYSROOT}/usr/lib)

# 生成可执行文件
add_executable(locker_agent ${SOURCES})
target_link_libraries(locker_agent paho-mqtt3c cjson pthread)
