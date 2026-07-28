#include "sync.h"

#include <process.h>
#include <stdint.h>
#include <stdlib.h>
#include <windows.h>

typedef struct {
    CRITICAL_SECTION native;
} WindowsMutex;

typedef struct {
    CONDITION_VARIABLE native;
} WindowsCondition;

typedef struct {
    SchedThreadFunction function;
    void *user_context;
    int result;
} WindowsThreadStart;

typedef struct {
    HANDLE handle;
    WindowsThreadStart *start;
} WindowsThread;

static unsigned __stdcall windows_thread_entry(void *argument)
{
    WindowsThreadStart *start = argument;

    start->result = start->function(start->user_context);
    return 0U;
}

SchedSyncResult sched_mutex_init(SchedMutex *mutex)
{
    WindowsMutex *implementation;

    if (mutex == NULL) {
        return SCHED_SYNC_ERROR_INVALID_ARGUMENT;
    }

    implementation = malloc(sizeof(*implementation));
    if (implementation == NULL) {
        return SCHED_SYNC_ERROR_SYSTEM;
    }

    if (!InitializeCriticalSectionEx(&implementation->native, 0U, 0U)) {
        free(implementation);
        return SCHED_SYNC_ERROR_SYSTEM;
    }

    mutex->implementation = implementation;
    return SCHED_SYNC_OK;
}

void sched_mutex_destroy(SchedMutex *mutex)
{
    WindowsMutex *implementation;

    if (mutex == NULL || mutex->implementation == NULL) {
        return;
    }

    implementation = mutex->implementation;
    DeleteCriticalSection(&implementation->native);
    free(implementation);
    mutex->implementation = NULL;
}

SchedSyncResult sched_mutex_lock(SchedMutex *mutex)
{
    WindowsMutex *implementation;

    if (mutex == NULL || mutex->implementation == NULL) {
        return SCHED_SYNC_ERROR_INVALID_ARGUMENT;
    }

    implementation = mutex->implementation;
    EnterCriticalSection(&implementation->native);
    return SCHED_SYNC_OK;
}

SchedSyncResult sched_mutex_unlock(SchedMutex *mutex)
{
    WindowsMutex *implementation;

    if (mutex == NULL || mutex->implementation == NULL) {
        return SCHED_SYNC_ERROR_INVALID_ARGUMENT;
    }

    implementation = mutex->implementation;
    LeaveCriticalSection(&implementation->native);
    return SCHED_SYNC_OK;
}

SchedSyncResult sched_condition_init(SchedCondition *condition)
{
    WindowsCondition *implementation;

    if (condition == NULL) {
        return SCHED_SYNC_ERROR_INVALID_ARGUMENT;
    }

    implementation = malloc(sizeof(*implementation));
    if (implementation == NULL) {
        return SCHED_SYNC_ERROR_SYSTEM;
    }

    InitializeConditionVariable(&implementation->native);
    condition->implementation = implementation;
    return SCHED_SYNC_OK;
}

void sched_condition_destroy(SchedCondition *condition)
{
    if (condition == NULL || condition->implementation == NULL) {
        return;
    }

    /*
     * Windows CONDITION_VARIABLE requires no native destruction operation.
     * Only the abstraction's private storage is released.
     */
    free(condition->implementation);
    condition->implementation = NULL;
}

SchedSyncResult sched_condition_wait(
    SchedCondition *condition,
    SchedMutex *mutex
)
{
    WindowsCondition *condition_implementation;
    WindowsMutex *mutex_implementation;

    if (condition == NULL
        || condition->implementation == NULL
        || mutex == NULL
        || mutex->implementation == NULL) {
        return SCHED_SYNC_ERROR_INVALID_ARGUMENT;
    }

    condition_implementation = condition->implementation;
    mutex_implementation = mutex->implementation;
    if (!SleepConditionVariableCS(
            &condition_implementation->native,
            &mutex_implementation->native,
            INFINITE
        )) {
        return SCHED_SYNC_ERROR_SYSTEM;
    }

    return SCHED_SYNC_OK;
}

SchedSyncResult sched_condition_signal(SchedCondition *condition)
{
    WindowsCondition *implementation;

    if (condition == NULL || condition->implementation == NULL) {
        return SCHED_SYNC_ERROR_INVALID_ARGUMENT;
    }

    implementation = condition->implementation;
    WakeConditionVariable(&implementation->native);
    return SCHED_SYNC_OK;
}

SchedSyncResult sched_condition_broadcast(SchedCondition *condition)
{
    WindowsCondition *implementation;

    if (condition == NULL || condition->implementation == NULL) {
        return SCHED_SYNC_ERROR_INVALID_ARGUMENT;
    }

    implementation = condition->implementation;
    WakeAllConditionVariable(&implementation->native);
    return SCHED_SYNC_OK;
}

SchedSyncResult sched_thread_create(
    SchedThread *thread,
    SchedThreadFunction function,
    void *context
)
{
    WindowsThread *implementation;
    WindowsThreadStart *start;
    uintptr_t native_handle;

    if (thread == NULL || function == NULL) {
        return SCHED_SYNC_ERROR_INVALID_ARGUMENT;
    }

    implementation = malloc(sizeof(*implementation));
    if (implementation == NULL) {
        return SCHED_SYNC_ERROR_SYSTEM;
    }

    start = malloc(sizeof(*start));
    if (start == NULL) {
        free(implementation);
        return SCHED_SYNC_ERROR_SYSTEM;
    }

    start->function = function;
    start->user_context = context;
    start->result = 0;

    native_handle = _beginthreadex(
        NULL,
        0U,
        windows_thread_entry,
        start,
        0U,
        NULL
    );
    if (native_handle == 0U) {
        free(start);
        free(implementation);
        return SCHED_SYNC_ERROR_SYSTEM;
    }

    implementation->handle = (HANDLE)native_handle;
    implementation->start = start;
    thread->implementation = implementation;
    return SCHED_SYNC_OK;
}

SchedSyncResult sched_thread_join(SchedThread *thread, int *result)
{
    WindowsThread *implementation;

    if (thread == NULL || thread->implementation == NULL) {
        return SCHED_SYNC_ERROR_INVALID_ARGUMENT;
    }

    implementation = thread->implementation;
    if (WaitForSingleObject(implementation->handle, INFINITE)
        != WAIT_OBJECT_0) {
        return SCHED_SYNC_ERROR_SYSTEM;
    }

    if (result != NULL) {
        *result = implementation->start->result;
    }

    return SCHED_SYNC_OK;
}

void sched_thread_destroy(SchedThread *thread)
{
    WindowsThread *implementation;

    if (thread == NULL || thread->implementation == NULL) {
        return;
    }

    implementation = thread->implementation;
    (void)CloseHandle(implementation->handle);
    free(implementation->start);
    free(implementation);
    thread->implementation = NULL;
}
