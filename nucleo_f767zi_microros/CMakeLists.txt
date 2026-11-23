cmake_minimum_required(VERSION 3.20.0)

# The micro-ROS clone (used via west) is NOT packaged as a west module at its
# repo root - its Kconfig/CMake live inside modules/libmicroros. Register that
# directory as an extra Zephyr module, exactly as the module's own sample app
# does (github.com/micro-ROS/micro_ros_zephyr_module, CMakeLists.txt).
#
# The second module, microros_compat, is a one-line include-path shim needed
# on Zephyr >= 3.4 (see its CMakeLists.txt for the full explanation).
list(APPEND ZEPHYR_EXTRA_MODULES
  ${CMAKE_CURRENT_SOURCE_DIR}/microros_compat
  ${CMAKE_CURRENT_SOURCE_DIR}/../modules/micro_ros_zephyr_module/modules/libmicroros)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(microros_nucleo_f767zi)

# Version string printed on the console at boot. CI passes the tag name (or
# "main-<sha>") with -DAPP_VERSION=..., so the firmware can be identified from
# the serial log alone.
set(APP_VERSION "dev" CACHE STRING "Firmware version string")
target_compile_definitions(app PRIVATE APP_VERSION="${APP_VERSION}")

target_sources(app PRIVATE
  src/main.c
  src/udp_transport.c
)
