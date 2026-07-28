#ifndef CONCURRENT_SCHEDULER_PLATFORM_SYNC_H
#define CONCURRENT_SCHEDULER_PLATFORM_SYNC_H

typedef enum {
    SCHED_SYNC_OK,
    SCHED_SYNC_ERROR_INVALID_ARGUMENT,
    SCHED_SYNC_ERROR_SYSTEM
} SchedSyncResult;

typedef struct {
    void *implementation;
} SchedMutex;

typedef struct {
    void *implementation;
} SchedCondition;

typedef int (*SchedThreadFunction)(void *context);

typedef struct {
    void *implementation;
} SchedThread;

SchedSyncResult sched_mutex_init(SchedMutex *mutex);
void sched_mutex_destroy(SchedMutex *mutex);
SchedSyncResult sched_mutex_lock(SchedMutex *mutex);
SchedSyncResult sched_mutex_unlock(SchedMutex *mutex);

SchedSyncResult sched_condition_init(SchedCondition *condition);
void sched_condition_destroy(SchedCondition *condition);
SchedSyncResult sched_condition_wait(
    SchedCondition *condition,
    SchedMutex *mutex
);
SchedSyncResult sched_condition_signal(SchedCondition *condition);
SchedSyncResult sched_condition_broadcast(SchedCondition *condition);

/*
 * Thread objects follow create -> join -> destroy. Destroy closes native
 * resources but does not terminate a running thread.
 */
SchedSyncResult sched_thread_create(
    SchedThread *thread,
    SchedThreadFunction function,
    void *context
);
SchedSyncResult sched_thread_join(SchedThread *thread, int *result);
void sched_thread_destroy(SchedThread *thread);

#endif
