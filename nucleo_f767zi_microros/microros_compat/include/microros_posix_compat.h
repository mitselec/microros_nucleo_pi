/* SPDX-License-Identifier: Apache-2.0
 *
 * Declarations that Zephyr/picolibc do not expose under the feature macros
 * the micro-ROS build uses. Force-included into every translation unit from
 * microros_compat/CMakeLists.txt, so the declarations reach the rcutils
 * sources without modifying the module.
 */

#ifndef MICROROS_POSIX_COMPAT_H_
#define MICROROS_POSIX_COMPAT_H_

#ifndef __ASSEMBLER__

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* picolibc implements these but only declares them for POSIX <= 2001. */
int strcasecmp(const char *s1, const char *s2);
int strncasecmp(const char *s1, const char *s2, size_t n);

/* Zephyr has no isatty() implementation for picolibc. rcutils calls it to
 * decide whether to colourise log output; src/posix_stubs.c supplies a weak
 * implementation that reports "not a tty". */
int isatty(int fd);

#ifdef __cplusplus
}
#endif

#endif /* __ASSEMBLER__ */

#endif /* MICROROS_POSIX_COMPAT_H_ */
