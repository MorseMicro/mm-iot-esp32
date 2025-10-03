/*
 * Copyright 2021-2023 Morse Micro
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Porting assistant self test tool to validate hardware and HALs.
 *
 * @note It is assumed that you have followed the steps in the @ref GETTING_STARTED guide and are
 * therefore familiar with how to build, flash, and monitor an application using the MM-IoT-SDK
 * framework.
 *
 * The Morse Micro Porting Assistant is a tool that assists with validating OSAL, HALs, and
 * platform hardware necessary for communicating with a Morse Micro chip. It runs a sequence of
 * tests and displays the results of each along with information about potential causes on failure.
 *
 * This tool has been provided as an example application on the Morse Micro reference platforms.
 * Running this application on a known-good platform should result in a 100% pass rate.
 *
 * When bringing up a new platform, it is recommended to first port this example application
 * to your new platform. It will then assist in diagnosing fundamental issues that may be
 * more difficult to identify or debug when using other example applications.
 */

#include "porting_assistant.h"

#if defined(ENABLE_EXT_XTAL_INIT) && ENABLE_EXT_XTAL_INIT
/* The crystal initialization requires chip specific configuration which we do not have access
 * to in the porting assistant example application. */
#error "With ENABLE_EXT_XTAL_INIT defined this device is not supported by porting assistant."
#endif

/* Test step declarations */
extern const struct test_step test_step_os_malloc;                      /**< Test definition */
extern const struct test_step test_step_os_realloc;                     /**< Test definition */
extern const struct test_step test_step_os_time;                        /**< Test definition */
extern const struct test_step test_step_os_task_creation;               /**< Test definition */

extern const struct test_step test_step_mmhal_wlan_init;                /**< Test definition */
extern const struct test_step test_step_mmhal_wlan_hard_reset;          /**< Test definition */
extern const struct test_step test_step_mmhal_wlan_sdio_startup;        /**< Test definition */
extern const struct test_step test_step_read_chip_id;                   /**< Test definition */
extern const struct test_step test_step_bulk_write_read;                /**< Test definition */
extern const struct test_step test_step_raw_tput;                       /**< Test definition */

extern const struct test_step test_step_mmhal_wlan_validate_fw;         /**< Test definition */
extern const struct test_step test_step_mmhal_wlan_validate_bcf;        /**< Test definition */

extern const struct test_step test_step_verify_busy_pin;                /**< Test definition */


/** Array of test steps. */
static const struct test_step * const test_steps[] = {
    &test_step_os_malloc,
    &test_step_os_realloc,
    &test_step_os_time,
    &test_step_os_task_creation,
    &test_step_mmhal_wlan_init,
    &test_step_mmhal_wlan_hard_reset,
    &test_step_mmhal_wlan_sdio_startup,
    &test_step_read_chip_id,
    &test_step_verify_busy_pin,
    &test_step_bulk_write_read,
    &test_step_raw_tput,
    &test_step_mmhal_wlan_validate_fw,
    &test_step_mmhal_wlan_validate_bcf,
};

/** Counters to track test runs. */
struct test_counters
{
    /** Number of tests that did not return a pass/fail result. */
    unsigned no_result;
    /** Number of tests that passed. */
    unsigned pass;
    /** Number of tests that failed. */
    unsigned fail;
};

/**
 * Convert a @c test_result code to string.
 *
 * @param result    The result code.
 *
 * @returns the string representation of the result code (including color if enabled).
 */
static const char *result_code_to_string(enum test_result result)
{
    switch (result)
    {
    case TEST_NO_RESULT:            return "";
    case TEST_PASSED:               return F_GREEN("PASS");
    case TEST_SKIPPED:              return F_BLUE("SKIP");
    case TEST_FAILED:               return F_RED("FAIL");
    case TEST_FAILED_NON_CRITICAL:  return F_YELLOW("FAIL");
    }
    MMOSAL_ASSERT(false);
}

/**
 * Iterate through the given list of test steps and execute until complete or until a
 * critical failure occurs.
 *
 * @param steps         Test steps to execute.
 * @param num_steps     Number of steps to execute.
 * @param ctrs          Test count state to be updated by this function.
 */
static void run_test_steps(const struct test_step * const steps[], size_t num_steps,
                           struct test_counters *ctrs)
{
    static char log_buf[1024];

    size_t ii;
    for (ii = 0; ii < num_steps; ii++)
    {
        const struct test_step *step = steps[ii];
        size_t log_buf_len = sizeof(log_buf);
        log_buf[0] = '\0';

        LOG_PRINTF(F_BOLD("%-60s "), step->description);
        LOG_FLUSH();

        enum test_result result = step->exec(log_buf, log_buf_len);
        if (result != TEST_NO_RESULT)
        {
            LOG_PRINTF("[ %s ]", result_code_to_string(result));
        }
        LOG_WRITE("\n");

        if (log_buf[0] != '\0')
        {
            LOG_WRITE("\n");
            LOG_WRITE(log_buf);
        }

        switch (result)
        {
        case TEST_NO_RESULT:            ctrs->no_result++;  break;
        case TEST_PASSED:               ctrs->pass++;       break;
        case TEST_SKIPPED:                                  break;
        case TEST_FAILED:               ctrs->fail++;       break;
        case TEST_FAILED_NON_CRITICAL:  ctrs->fail++;       break;
        }

        if (result == TEST_FAILED)
        {
            break;
        }
    }
}

/**
 * Main entry point to the application. This will be invoked in a thread once operating system
 * and hardware initialization has completed. It may return, but it does not have to.
 */
void app_main(void)
{
    mmhal_init();
    /* Having deep sleep enabled can complicate debugging. Given the purpose of this applications
     * is to validate that you can communicate to the MM-Chip it will be disabled by default. */
    mmhal_set_deep_sleep_veto(MMHAL_VETO_ID_APP_MIN);

    struct test_counters ctrs = { 0 };
    unsigned num_tests = sizeof(test_steps)/sizeof(test_steps[0]);

    LOG_WRITE(F_BOLD("\n\nMM-IoT-SDK Porting Assistant\n"));
    LOG_WRITE("----------------------------\n\n");
    run_test_steps(test_steps, num_tests, &ctrs);


    LOG_PRINTF("\n\n%u total test steps. %u passed, %u failed, %u no result, %u skipped\n",
               num_tests, ctrs.pass, ctrs.fail, ctrs.no_result,
               num_tests - ctrs.no_result - ctrs.pass - ctrs.fail);
}
