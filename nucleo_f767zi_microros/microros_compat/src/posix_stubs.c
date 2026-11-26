/* SPDX-License-Identifier: Apache-2.0 */

#include <unistd.h>

#if defined(CONFIG_PICOLIBC)
/*
 * Zephyr does not link picolibc's optional semihost library, which is where
 * isatty() lives. rcutils only calls it to decide whether to emit ANSI
 * colours, which is meaningless on this board. Keep the definition weak so a
 * real implementation wins if one is ever linked.
 */
__attribute__((weak)) int isatty(int fd)
{
	(void)fd;
	return 0;
}
#endif
