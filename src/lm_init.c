/*
 * statically linked program to test live migration,
 * it's supposed to be packaged into an initrd.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#define TEST_MEM_SIZE (32 * 1024 * 1024)  /* 32 MB */
#define PAGE_SIZE     4096
#define LOOPS_PER_MSG 64

int main(void)
{
    uint8_t *mem;
    int fd, pass = 0;

    fd = open("/dev/console", O_WRONLY);
    if (fd < 0) {
        fd = STDOUT_FILENO;
    }

    write(fd, "INIT:READY\n", 11);

    mem = malloc(TEST_MEM_SIZE);
    if (!mem) {
        write(fd, "E", 1);
        for (;;) {
            pause();
        }
    }
    memset(mem, 0, TEST_MEM_SIZE);

    /* dirty pages in a loop, like the bootblock a-b test */
    for (;;) {
        uint8_t *p;
        for (p = mem; p < mem + TEST_MEM_SIZE; p += PAGE_SIZE) {
            (*p)++;
        }
        if (++pass % LOOPS_PER_MSG == 0) {
            write(fd, "INIT:ALIVE\n", 11);
        }
    }

    return 0;
}
