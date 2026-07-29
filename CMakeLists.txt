cmake_minimum_required(VERSION 3.20)

project(
    concurrent-task-scheduling-engine
    VERSION 0.1.0
    DESCRIPTION "Concurrent Task Scheduling Engine in C"
    LANGUAGES C
)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

include(CTest)

option(
    CONCURRENT_SCHEDULER_ENABLE_PROFILING
    "Enable benchmark-private contention profiling diagnostics"
    OFF
)
option(
    CONCURRENT_SCHEDULER_USE_TRANSITION_SIGNALING
    "Use transition-aware queue condition signaling"
    OFF
)

function(concurrent_scheduler_enable_warnings target)
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(
            "${target}"
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
                -Wconversion
        )
    elseif(MSVC)
        target_compile_options("${target}" PRIVATE /W4)
    endif()
endfunction()

add_library(
    concurrent_scheduler_core
    src/concurrent_scheduler.c
    src/concurrent_task_queue.c
    src/scheduler.c
    src/scheduler_validation.c
    src/task.c
    src/task_queue.c
)

target_include_directories(
    concurrent_scheduler_core
    PUBLIC
        "${PROJECT_SOURCE_DIR}/include"
)

target_compile_definitions(
    concurrent_scheduler_core
    PRIVATE
        CONCURRENT_SCHEDULER_VERSION="${PROJECT_VERSION}"
        CONCURRENT_SCHEDULER_USE_TRANSITION_SIGNALING=$<BOOL:${CONCURRENT_SCHEDULER_USE_TRANSITION_SIGNALING}>
)

concurrent_scheduler_enable_warnings(concurrent_scheduler_core)

add_executable(
    concurrent-task-scheduling-engine
    src/main.c
)

target_link_libraries(
    concurrent-task-scheduling-engine
    PRIVATE
        concurrent_scheduler_core
)

concurrent_scheduler_enable_warnings(concurrent-task-scheduling-engine)

add_executable(
    concurrent_scheduler_tests
    tests/test_concurrent_scheduler.c
)

target_link_libraries(
    concurrent_scheduler_tests
    PRIVATE
        concurrent_scheduler_core
)

target_compile_definitions(
    concurrent_scheduler_tests
    PRIVATE
        CONCURRENT_SCHEDULER_EXPECTED_VERSION="${PROJECT_VERSION}"
)

concurrent_scheduler_enable_warnings(concurrent_scheduler_tests)

add_test(
    NAME concurrent_scheduler.foundation
    COMMAND concurrent_scheduler_tests
)

add_executable(
    concurrent_scheduler_task_tests
    tests/test_task.c
)

target_link_libraries(
    concurrent_scheduler_task_tests
    PRIVATE
        concurrent_scheduler_core
)

concurrent_scheduler_enable_warnings(concurrent_scheduler_task_tests)

add_test(
    NAME concurrent_scheduler.task_domain
    COMMAND concurrent_scheduler_task_tests
)

add_executable(
    concurrent_scheduler_task_queue_tests
    tests/test_task_queue.c
)

target_link_libraries(
    concurrent_scheduler_task_queue_tests
    PRIVATE
        concurrent_scheduler_core
)

concurrent_scheduler_enable_warnings(concurrent_scheduler_task_queue_tests)

add_test(
    NAME concurrent_scheduler.task_queue
    COMMAND concurrent_scheduler_task_queue_tests
)

if(WIN32)
    add_library(
        concurrent_scheduler_platform_sync
        STATIC
        src/platform/windows/sync_windows.c
    )

    target_include_directories(
        concurrent_scheduler_platform_sync
        PRIVATE
            "${PROJECT_SOURCE_DIR}/src/platform"
    )

    target_compile_definitions(
        concurrent_scheduler_platform_sync
        PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
    )

    concurrent_scheduler_enable_warnings(
        concurrent_scheduler_platform_sync
    )

    target_include_directories(
        concurrent_scheduler_core
        PRIVATE
            "${PROJECT_SOURCE_DIR}/src"
    )

    target_link_libraries(
        concurrent_scheduler_core
        PRIVATE
            concurrent_scheduler_platform_sync
    )

    add_executable(
        concurrent_scheduler_synchronization_backend_tests
        tests/synchronization_backend_test.c
    )

    target_include_directories(
        concurrent_scheduler_synchronization_backend_tests
        PRIVATE
            "${PROJECT_SOURCE_DIR}/src/platform"
    )

    target_link_libraries(
        concurrent_scheduler_synchronization_backend_tests
        PRIVATE
            concurrent_scheduler_platform_sync
    )

    concurrent_scheduler_enable_warnings(
        concurrent_scheduler_synchronization_backend_tests
    )

    add_test(
        NAME concurrent_scheduler.synchronization_backend
        COMMAND concurrent_scheduler_synchronization_backend_tests
    )

    add_executable(
        concurrent_scheduler_concurrent_task_queue_tests
        tests/test_concurrent_task_queue.c
    )

    target_include_directories(
        concurrent_scheduler_concurrent_task_queue_tests
        PRIVATE
            "${PROJECT_SOURCE_DIR}/src/platform"
    )

    target_link_libraries(
        concurrent_scheduler_concurrent_task_queue_tests
        PRIVATE
            concurrent_scheduler_core
            concurrent_scheduler_platform_sync
    )

    concurrent_scheduler_enable_warnings(
        concurrent_scheduler_concurrent_task_queue_tests
    )

    add_test(
        NAME concurrent_scheduler.concurrent_task_queue
        COMMAND concurrent_scheduler_concurrent_task_queue_tests
    )

    add_executable(
        concurrent_scheduler_scheduler_tests
        tests/test_scheduler.c
    )

    target_include_directories(
        concurrent_scheduler_scheduler_tests
        PRIVATE
            "${PROJECT_SOURCE_DIR}/src/platform"
    )

    target_link_libraries(
        concurrent_scheduler_scheduler_tests
        PRIVATE
            concurrent_scheduler_core
            concurrent_scheduler_platform_sync
    )

    concurrent_scheduler_enable_warnings(
        concurrent_scheduler_scheduler_tests
    )

    add_test(
        NAME concurrent_scheduler.scheduler
        COMMAND concurrent_scheduler_scheduler_tests
    )

    add_executable(
        concurrent_scheduler_observability_tests
        tests/test_scheduler_observability.c
    )

    target_include_directories(
        concurrent_scheduler_observability_tests
        PRIVATE
            "${PROJECT_SOURCE_DIR}/src"
            "${PROJECT_SOURCE_DIR}/src/platform"
    )

    target_link_libraries(
        concurrent_scheduler_observability_tests
        PRIVATE
            concurrent_scheduler_core
            concurrent_scheduler_platform_sync
    )

    concurrent_scheduler_enable_warnings(
        concurrent_scheduler_observability_tests
    )

    add_test(
        NAME concurrent_scheduler.observability
        COMMAND concurrent_scheduler_observability_tests
    )

    add_executable(
        concurrent_scheduler_benchmarks
        benchmarks/benchmark_scheduler.c
        benchmarks/benchmark_timer_windows.c
    )

    target_include_directories(
        concurrent_scheduler_benchmarks
        PRIVATE
            "${PROJECT_SOURCE_DIR}/benchmarks"
            "${PROJECT_SOURCE_DIR}/src"
            "${PROJECT_SOURCE_DIR}/src/platform"
    )

    target_link_libraries(
        concurrent_scheduler_benchmarks
        PRIVATE
            concurrent_scheduler_core
            concurrent_scheduler_platform_sync
    )

    target_compile_definitions(
        concurrent_scheduler_benchmarks
        PRIVATE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
            BENCHMARK_VERSION="${PROJECT_VERSION}"
    )

    if(CONCURRENT_SCHEDULER_ENABLE_PROFILING)
        target_compile_definitions(
            concurrent_scheduler_core
            PRIVATE
                CONCURRENT_SCHEDULER_ENABLE_PROFILING=1
        )
        target_compile_definitions(
            concurrent_scheduler_benchmarks
            PRIVATE
                CONCURRENT_SCHEDULER_ENABLE_PROFILING=1
                CONCURRENT_SCHEDULER_PROFILING_BUILD=1
        )
    endif()

    concurrent_scheduler_enable_warnings(
        concurrent_scheduler_benchmarks
    )
else()
    message(
        FATAL_ERROR
        "No synchronization backend is available for this platform."
    )
endif()
