#pragma once

#include <stdio.h>

#define LINE_DEBUG(MSG) printf("%s:%d : %s\n", __func__, __LINE__, MSG);
#define LOG(...) fprintf(stdout, __VA_ARGS__)
#define ERROR(...) fprintf(stderr, __VA_ARGS__)

void fatal(char *str);
void error(char *str);
int eopen(const char *path, int flag);
void *emmap(int addr, size_t len, int prot, int flag, int fd, off_t offset);
