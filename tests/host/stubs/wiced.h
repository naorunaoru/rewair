#pragma once

#include <pthread.h>
#include <stdint.h>

typedef int wiced_result_t;
typedef uint32_t wiced_utc_time_t;
typedef pthread_mutex_t wiced_mutex_t;

#define WICED_SUCCESS 0
#define WICED_ERROR   1
#define WICED_BADARG  2
#define WICED_FALSE   0

wiced_result_t wiced_rtos_init_mutex( wiced_mutex_t* mutex );
wiced_result_t wiced_rtos_lock_mutex( wiced_mutex_t* mutex );
wiced_result_t wiced_rtos_unlock_mutex( wiced_mutex_t* mutex );
wiced_result_t wiced_time_get_utc_time( wiced_utc_time_t* utc_time );
