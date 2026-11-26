/* SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr 4.3 removed include/zephyr/posix/time.h and replaced it with
 * posix_time.h. micro-ROS/rcutils (src/time_unix.c) still includes the old
 * name on every Zephyr >= 3.1, so the libmicroros build fails on 4.3+ with
 *     fatal error: zephyr/posix/time.h: No such file or directory
 *
 * Forward to the real header when it exists (Zephyr <= 4.2). On 4.3+ there is
 * no forwarding header, and posix_time.h is not a drop-in replacement here:
 * Zephyr defines _POSIX_C_SOURCE=200809L for the micro-ROS build, so both
 * newlib and picolibc already declare clock_gettime(), struct itimerspec,
 * timer_gettime()/timer_settime() and locale_t in their own headers. Including
 * posix_time.h on top redefines all of them, which is a hard error. The C
 * library header is enough, so use it directly.
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
#endif

#endif /* MICROROS_COMPAT_ZEPHYR_POSIX_TIME_H_ */
