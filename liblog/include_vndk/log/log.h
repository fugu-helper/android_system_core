/*Special log.h file for VNDK linking modules*/

#ifndef _LIBS_LOG_LOG_H
#define _LIBS_LOG_LOG_H
#if !defined(_WIN32)
#include <pthread.h>
#endif
#include <stdint.h> /* uint16_t, int32_t */
#include <stdio.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <android/log.h>
#include <log/log_id.h>
#include <log/log_main.h>
#include <log/log_radio.h>
#include <log/log_read.h>
#include <log/log_safetynet.h>
#include <log/log_system.h>
#include <log/log_time.h>

/*
 * LOG_TAG is the local tag used for the following simplified
 * logging macros.  You can change this preprocessor definition
 * before using the other macros to change the tag.
 */

#ifndef LOG_TAG
#define LOG_TAG NULL
#endif

#endif /*_LIBS_LOG_LOG_H*/
