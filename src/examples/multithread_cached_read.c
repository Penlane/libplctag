#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>
#include <inttypes.h>
#include <sys/time.h>
#include "../lib/libplctag.h"
#include "utils.h"

#define TAG_PATH "protocol=ab_eip&gateway=10.17.45.37&path=1,0&cpu=LGX&elem_size=4&elem_count=1&name=DataIn_Frm_Sched[1]&read_cache_ms=100"
#define ELEM_COUNT 1
#define ELEM_SIZE 4
#define DATA_TIMEOUT 500

#define MAX_THREADS (300)


/*
 * This test program creates a lot of threads that read the same tag in
 * the plc.  They all hit the exact same underlying tag data structure.
 * This tests, to some extent, whether the library can handle multi-threaded
 * access.
 */


/* global to cheat on passing it to threads. */
tag_id tag;



/*
 * Thread function.  Just read until killed.
 */

void *thread_func(void *data)
{
    int tid = (int)(intptr_t)data;
    int rc;
    int value;
    uint64_t start;
    uint64_t end;

    while(1) {
        /* capture the starting time */
        start = time_ms();

        /* use do/while to allow easy exit without return */
        do {
            rc = plc_tag_lock(tag);

            if(rc != PLCTAG_STATUS_OK) {
                value = 1000;
                break; /* punt, no lock */
            }

            rc = plc_tag_read(tag, DATA_TIMEOUT);

            if(rc != PLCTAG_STATUS_OK) {
                value = 1001;
            } else {
                value = (int)plc_tag_get_int32(tag,0);
            }

            /* yes, we should look at the return value */
            plc_tag_unlock(tag);
        } while(0);

        end = time_ms();

        fprintf(stderr,"%" PRId64 " Thread %d got result %d with return code %s in %dms\n",time_ms(),tid,value,plc_tag_decode_error(rc),(int)(end-start));

        sleep_ms(10);
    }

    return NULL;
}


int main(int argc, char **argv)
{
    pthread_t thread[MAX_THREADS];
    int num_threads;
    int thread_id = 0;

    if(argc != 2) {
        fprintf(stderr,"ERROR: Must provide number of threads to run (between 1 and 300) argc=%d!\n",argc);
        return 0;
    }

    num_threads = (int)strtol(argv[1],NULL, 10);

    if(num_threads < 1 || num_threads > MAX_THREADS) {
        fprintf(stderr,"ERROR: %d (%s) is not a valid number. Must provide number of threads to run (between 1 and 300)!\n",num_threads, argv[1]);
        return 0;
    }

    /* create the tag */
    tag = plc_tag_create(TAG_PATH, DATA_TIMEOUT);

    /* everything OK? */
    if(tag < 0) {
        fprintf(stderr,"ERROR: Could not create tag! Error %s\n", plc_tag_decode_error(tag));

        return 0;
    }

    /* let the connect succeed we hope */
    while(plc_tag_status(tag) == PLCTAG_STATUS_PENDING) {
        sleep_ms(100);
    }

    if(plc_tag_status(tag) != PLCTAG_STATUS_OK) {
        fprintf(stderr,"Error setting up tag internal state.\n");
        return 0;
    }


    /* create the read threads */

    fprintf(stderr,"Creating %d threads.\n",num_threads);

    for(thread_id=0; thread_id < num_threads; thread_id++) {
        pthread_create(&thread[thread_id], NULL, &thread_func, (void *)(intptr_t)thread_id);
    }

    /* wait until ^C */
    while(1) {
        sleep_ms(100);
    }

    return 0;
}

