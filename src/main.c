#include "concurrent_scheduler/concurrent_scheduler.h"

#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    printf(
        "========================================\n"
        "Concurrent Task Scheduling Engine in C\n"
        "Version: %s\n"
        "Status: initialized\n"
        "========================================\n",
        concurrent_scheduler_version()
    );

    return EXIT_SUCCESS;
}
