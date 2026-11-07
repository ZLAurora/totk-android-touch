/* Run production startup without Android APIs, clocks on the off arm, or threads. */
#define __ANDROID__ 1
#define TOTK_ANDROID_APP 1
#include <pthread.h>
#include <errno.h>
#include <assert.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
static unsigned starts;
static int tracked_create(pthread_t *thread, const pthread_attr_t *attributes,
        void *(*entry)(void *), void *argument) {
    (void)thread; (void)attributes; (void)entry; (void)argument;
    starts++;
    return EAGAIN; /* prove admission, without starting an OS thread */
}
#define pthread_create tracked_create
#include "../games/totk/runtime/android_thermal.c"
int main(void) {
    const char *values[] = {NULL, "0", "", "invalid", "01", "1"};
    for (unsigned i = 0; i < sizeof values / sizeof *values; i++) {
        pid_t child = fork();
        assert(child >= 0);
        if (!child) {
            if (values[i]) setenv("TOTK_THERMAL_STEP", values[i], 1);
            else unsetenv("TOTK_THERMAL_STEP");
            for (unsigned frame = 0; frame < 300; frame++) runtime_thermal_frame(1);
            assert(starts == (i == 5));
            assert(frames == 0 && transitions == 0);
            _exit(0);
        }
        int result;
        assert(waitpid(child, &result, 0) == child);
        assert(WIFEXITED(result) && WEXITSTATUS(result) == 0);
    }
    return 0;
}
