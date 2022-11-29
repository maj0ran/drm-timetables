#include <stdint.h>
#include <stdlib.h>
#include <sys/fcntl.h>
#include <sys/mman.h>

#include <utils.h>

void error(char *str) {
  perror(str);
}

int eopen(const char *path, int flag) {
  int fd;

  if ((fd = open(path, flag)) < 0) {
    fprintf(stderr, "cannot open \"%s\"\n", path);
    error("[open]:");
  }
  return fd;
}

void *emmap(int addr, size_t len, int prot, int flag, int fd, off_t offset) {
  uint32_t *fp;

  (void)addr;

  if ((fp = (uint32_t *)mmap(0, len, prot, flag, fd, offset)) == MAP_FAILED)
    error("mmap");
  return fp;
}
