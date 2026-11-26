# SPDX-License-Identifier: Apache-2.0
#
# Zephyr >= 3.4 relocated the POSIX headers from `include/posix/` to
# `include/zephyr/posix/` (https://github.com/zephyrproject-rtos/zephyr).
# The micro-ROS module's UDP transport still uses the 2022-style
# `#include <posix/sys/socket.h>` (untouched upstream since 2022-12, in
# ALL micro_ros_zephyr_module branches incl. rolling), so on Zephyr 4.1
# compilation of modules/libmicroros/.../microros_transports/udp/* and any
# source including microros_transports.h fails with
#     fatal error: posix/sys/socket.h: No such file or directory
#
# Fix: put `include/zephyr` on the include path as well - then the old-style
# name resolves to the file's new location instead of forking the module.
#
# NOTE on scope: `zephyr_include_directories()` called from an *application*
# CMakeLists only affects the 'app' target, but called from a *module*
# CMakeLists it applies globally (all in-tree Zephyr libraries too). That is
# why this shim exists as its own tiny module and is registered via
# ZEPHYR_EXTRA_MODULES in the application CMakeLists.txt.
zephyr_include_directories(${ZEPHYR_BASE}/include/zephyr)

# Zephyr 4.3 split include/zephyr/posix/time.h into posix_time.h. rcutils still
# includes the old name, so forward it (see include/zephyr/posix/time.h).
zephyr_include_directories(${CMAKE_CURRENT_SOURCE_DIR}/include)

# rcutils asks for Annex K by defining __STDC_WANT_LIB_EXT1__ before including
# <string.h>. picolibc only declares __errno_t and __rsize_t in sys/_types.h
# when that macro is set on the header's first inclusion. By the time rcutils
# reaches <string.h>, <stdlib.h>/<stdio.h> have already pulled sys/_types.h in
# without the macro, so the later inclusion is skipped by the include guard and
# the typedefs are missing. rcutils only uses memmove_s() when the libc defines
# __STDC_LIB_EXT1__, which picolibc does not, so turning the request off makes
# it fall back to memmove() and avoids the broken header path.
if(CONFIG_PICOLIBC)
  zephyr_compile_definitions(__STDC_WANT_LIB_EXT1__=0)
endif()

# rcutils' process.c calls fork() and execvp(). Zephyr implements neither, and
# its POSIX headers do not declare them, so the implicit declarations become
# errors on Zephyr 4.4. The micro-ROS stack never calls the rcutils process
# API, so compile that implementation out instead of adding two more stubs.
zephyr_compile_definitions(RCUTILS_NO_PROCESS_SUPPORT)

# picolibc only declares strcasecmp()/strncasecmp() for POSIX <= 2001, and
# Zephyr does not declare isatty() for picolibc at all. rcutils uses all three,
# so force the declarations in (see include/microros_posix_compat.h). The
# header is empty for assembly, and redeclaring an existing prototype is
# harmless, so this is safe for newlib builds as well.
zephyr_compile_options(
  -include ${CMAKE_CURRENT_SOURCE_DIR}/include/microros_posix_compat.h)

zephyr_library()
zephyr_library_sources(src/posix_stubs.c)
