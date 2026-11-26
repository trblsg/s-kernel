#ifndef P12741_TEST_SYSCTL_H
#define P12741_TEST_SYSCTL_H
#include <stddef.h>
int sysctlbyname(const char *name,void *oldp,size_t *oldlenp,void *newp,size_t newlen);
#endif
