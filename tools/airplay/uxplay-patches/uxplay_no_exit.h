/* GCAST: never let UxPlay kill misterplexd. -include when building libairplay. */
#pragma once
#include <pthread.h>
#include <stdio.h>
static inline void uxplay_no_exit(int code) {
    fprintf(stderr, "airplay: refusing process exit(%d) from UxPlay; ending AirPlay thread only\n", code);
    pthread_exit(NULL);
}
#define exit(code) uxplay_no_exit(code)
