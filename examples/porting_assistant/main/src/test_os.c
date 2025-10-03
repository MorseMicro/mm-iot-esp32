/*
 * Copyright 2023 Morse Micro
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "porting_assistant.h"

TEST_STEP(test_step_os_malloc, "Memory allocation")
{
    void *buf = mmosal_malloc(1560);
    if (buf == NULL)
    {
        TEST_LOG_APPEND(
                 "Failed to allocate 1560 bytes. Check that your heap is configured correctly\n\n");
        return TEST_FAILED;
    }
    mmosal_free(buf);
    return TEST_PASSED;
}

TEST_STEP(test_step_os_realloc, "Memory reallocation")
{
    enum constants
    {
        FIRST_ALLOCATION_SIZE = 100,
        REALLOCATION_SIZE     = 200,
    };

    uint8_t *buf = (uint8_t *)mmosal_malloc(FIRST_ALLOCATION_SIZE);
    if (buf == NULL)
    {
        TEST_LOG_APPEND(
                 "Failed to allocate %d bytes. Check that your heap is configured correctly\n\n",
                 FIRST_ALLOCATION_SIZE);
        return TEST_FAILED_NON_CRITICAL;
    }
    memset(buf, 0xc0, FIRST_ALLOCATION_SIZE);

    uint8_t *buf1 = (uint8_t *)mmosal_realloc(buf, REALLOCATION_SIZE);
    if (buf1 == NULL)
    {
        TEST_LOG_APPEND(
                 "Failed to reallocate %d bytes. Check that your heap supports realloc\n\n",
                 REALLOCATION_SIZE);
        return TEST_FAILED_NON_CRITICAL;
    }

    /* Verify the reallocated memory contains the contents of the original block. */
    unsigned ii;
    for (ii = 0; ii < FIRST_ALLOCATION_SIZE; ii++)
    {
        if (buf1[ii] != 0xc0)
        {
            TEST_LOG_APPEND(
                     "Reallocated block contents mismatch at offset %u\n\n", ii);
            return TEST_FAILED_NON_CRITICAL;
        }
    }

    mmosal_free(buf1);
    return TEST_PASSED;
}

TEST_STEP(test_step_os_time, "Passage of time")
{
    uint32_t start_time = mmosal_get_time_ms();
    mmosal_task_sleep(50);
    uint32_t end_time = mmosal_get_time_ms();

    int32_t delta = (int32_t)(end_time - start_time);
    if (delta < 49 || delta > 51)
    {
        TEST_LOG_APPEND(
                 "Time delta (%ld ms) did not match sleep time (50 ms)\n\n", delta);
        return TEST_FAILED;
    }

    return TEST_PASSED;
}

/** Enumeration of task states for the task that is created during the task creation/preemption
 *  test. */
enum task_state
{
    TASK_NOT_STARTED,
    TASK_STARTED,
    TASK_TERMINATING,
    TASK_ERROR_GET_ACTIVE_INVALID,
};

/** Current state of the task that is created during the task creation/preemption test. */
static volatile enum task_state task_state = TASK_NOT_STARTED;
/** Handle of the task that is created during the task creation/preemption test. */
static struct mmosal_task * volatile task_handle;

/** Main function of the task that is created during the task creation/preemption test. */
static void new_task_main(void *arg)
{
    (void)arg;

    task_state = TASK_STARTED;

    /* Sleep for 10 ms; this should yield the task. */
    mmosal_task_sleep(10);

    /* Verify mmosal_task_get_active() returns the correct task handle. */
    if (mmosal_task_get_active() != task_handle)
    {
        task_state = TASK_ERROR_GET_ACTIVE_INVALID;
        return;
    }

    task_state = TASK_TERMINATING;

    /* Delete self */
    mmosal_task_delete(NULL);
}

TEST_STEP(test_step_os_task_creation, "Task creation and preemption")
{
    task_handle = mmosal_task_create(
        new_task_main, NULL, MMOSAL_TASK_PRI_HIGH, 512, "Test Task");
    if (task_handle == NULL)
    {
        TEST_LOG_APPEND(
                 "mmosal_task_create() returned NULL; expected a task handle.\n\n");
        return TEST_FAILED_NON_CRITICAL;
    }

    /*
     * The newly created task should be higher priority. Therefore the following code should
     * not run until after the task yields.
     */
    if (task_state != TASK_STARTED)
    {
        TEST_LOG_APPEND(
                 "The task created with mmosal_task_create() did not run.\n\n");
        return TEST_FAILED_NON_CRITICAL;
    }

    /* Allow some time for the task to wake up. */
    mmosal_task_sleep(50);

    switch (task_state)
    {
    case TASK_TERMINATING:
        break;

    case TASK_ERROR_GET_ACTIVE_INVALID:
        TEST_LOG_APPEND(
                 "mmosal_task_get_active() did not return the correct task handle.\n\n");
        return TEST_FAILED_NON_CRITICAL;

    default:
        TEST_LOG_APPEND(
                 "Task in unexpected state %d.\n\n", task_state);
        return TEST_FAILED_NON_CRITICAL;
    }

    return TEST_PASSED;
}
