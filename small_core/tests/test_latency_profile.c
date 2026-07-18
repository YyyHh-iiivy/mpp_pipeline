#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "latency_profile.h"

int main(void)
{
    latency_profile profile;
    char summary[1024];

    latency_profile_init(&profile);
    assert(latency_profile_format(&profile, summary, sizeof(summary)) == 0);

    latency_profile_observe(&profile, 10, 2, 3, 4, 100, 120);
    latency_profile_observe(&profile, 30, 4, 5, 8, 200, 240);
    assert(latency_profile_format(&profile, summary, sizeof(summary)) > 0);
    assert(strstr(summary, "count=2") != NULL);
    assert(strstr(summary, "msg_age_avg_ms=20") != NULL);
    assert(strstr(summary, "msg_age_p90_ms=30") != NULL);
    assert(strstr(summary, "send_max_ms=8") != NULL);
    assert(strstr(summary, "outq_after_max=240") != NULL);

    puts("latency profile tests passed");
    return 0;
}
