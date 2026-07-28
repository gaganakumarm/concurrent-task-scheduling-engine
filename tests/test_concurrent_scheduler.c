#include "concurrent_scheduler/concurrent_scheduler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CONCURRENT_SCHEDULER_EXPECTED_VERSION
#error "CONCURRENT_SCHEDULER_EXPECTED_VERSION must be defined by the build system"
#endif

int main(void)
{
    const char *version = concurrent_scheduler_version();

    if (version == NULL) {
        fprintf(stderr, "Version API returned NULL.\n");
        return EXIT_FAILURE;
    }

    if (version[0] == '\0') {
        fprintf(stderr, "Version API returned an empty string.\n");
        return EXIT_FAILURE;
    }

    if (strcmp(version, CONCURRENT_SCHEDULER_EXPECTED_VERSION) != 0) {
        fprintf(
            stderr,
            "Version mismatch: expected %s, received %s.\n",
            CONCURRENT_SCHEDULER_EXPECTED_VERSION,
            version
        );
        return EXIT_FAILURE;
    }

    printf("Version API test passed: %s\n", version);
    return EXIT_SUCCESS;
}
