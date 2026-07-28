#include "concurrent_scheduler/concurrent_scheduler.h"

#ifndef CONCURRENT_SCHEDULER_VERSION
#error "CONCURRENT_SCHEDULER_VERSION must be defined by the build system"
#endif

const char *concurrent_scheduler_version(void)
{
    return CONCURRENT_SCHEDULER_VERSION;
}
