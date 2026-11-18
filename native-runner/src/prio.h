/* The scheduling priority of the runner.
 *
 * The runner shares the Y1's CPU with Android. While the wheel turns,
 * Android's input work, the launcher and system scripts run on the same
 * core at the same priority, and the guest got too little time: its audio
 * came too slowly and the app's audio buffer ran empty (the music
 * stuttered). At nice -10 the guest kept real time in the same test, and
 * the buffer kept at least 249 ms. The runner runs as root (su), so it may
 * raise its own priority. RUNNER_NICE=<n> sets another value, 0 the normal
 * priority. */
#ifndef PRIO_H
#define PRIO_H
#include <stdlib.h>

#define RUNNER_NICE_DEFAULT (-10)

/* the nice value for RUNNER_NICE (NULL: not set), in -20..19 */
static inline int runner_nice_value(const char *e) {
    char *end;
    long v;
    if (!e || !*e) return RUNNER_NICE_DEFAULT;
    v = strtol(e, &end, 10);
    if (*end) return RUNNER_NICE_DEFAULT;
    return v < -20 ? -20 : v > 19 ? 19 : (int)v;
}

#endif
