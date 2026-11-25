/* SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr 4.3 removed include/zephyr/posix/time.h and replaced it with
 * posix_time.h. micro-ROS/rcutils (src/time_unix.c) still includes the old
 * name on every Zephyr >= 3.1, so the libmicroros build fails on 4.3+ with
 *     fatal error: zephyr/posix/time.h: No such file or directory
 *
 * Forward to the real header when it exists (Zephyr <= 4.2), and to the
 * replacement otherwise. posix_time.h expects the C library's <time.h> to
 * have defined struct timespec already.
 */
#ifndef MICROROS_COMPAT_ZEPHYR_POSIX_TIME_H_
#define MICROROS_COMPAT_ZEPHYR_POSIX_TIME_H_

#if defined(__has_include_next)
#if __has_include_next(<zephyr/posix/time.h>)
#include_next <zephyr/posix/time.h>
#define MICROROS_COMPAT_POSIX_TIME_FORWARDED
#endif
#endif

#ifndef MICROROS_COMPAT_POSIX_TIME_FORWARDED
#include <time.h>
#include <zephyr/posix/posix_time.h>
#endif

#endif /* MICROROS_COMPAT_ZEPHYR_POSIX_TIME_H_ */
