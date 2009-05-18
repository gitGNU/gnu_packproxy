#ifndef LOG_H
#define LOG_H

#include <stdio.h>

#define log(fmt, ...)						\
  printf (__FILE__ ":%d: " fmt "\n", __LINE__, ## __VA_ARGS__)

#endif
