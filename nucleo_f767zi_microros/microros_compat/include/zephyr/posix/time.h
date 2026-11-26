/* SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr 4.3 removed include/zephyr/posix/time.h and replaced it with
 * posix_time.h. micro-ROS/rcutils (src/time_unix.c) still includes the old
 * name on every Zephyr >= 3.1, so the libmicroros build fails on 4.3+ with
 *     fatal error: zephyr/posix/time.h: No such file or directory
 *
 * Forward to the real header when it exists (Zephyr <= 4.2). On 4.3+ there is
 * no forwarding header: newlib already declares clock_gettime() and the timer
 * functions in its own <time.h>, so including posix_time.h as well would only
 * duplicate struct itimerspec. picolibc needs posix_time.h because it does not
 * declare those functions itself.
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
#if !defined(CONFIG_NEWLIB_LIBC)
#include <zephyr/posix/posix_time.h>
#endif
#endif

#endif /* MICROROS_COMPAT_ZEPHYR_POSIX_TIME_H_ */
