#ifndef LOG_H
#define LOG_H

#include <stdio.h>

#define BOLD(text) "\033[01;31m" text "\033[00m"
#define log(fmt, ...)						\
  printf (__FILE__ ":%d: " fmt "\n", __LINE__, ## __VA_ARGS__)

#endif
