#ifndef LATENCY_PROFILE_H
#define LATENCY_PROFILE_H

#include <stddef.h>
#include <stdint.h>

#define LATENCY_PROFILE_WINDOW 64U

typedef struct {
    uint64_t total;
    uint64_t sum;
    uint64_t max;
    uint64_t samples[LATENCY_PROFILE_WINDOW];
    size_t count;
    size_t next;
} latency_profile_metric;

typedef struct {
    latency_profile_metric msg_age_ms;
    latency_profile_metric copy_ms;
    latency_profile_metric read_done_ms;
    latency_profile_metric send_ms;
    latency_profile_metric outq_before;
    latency_profile_metric outq_after;
} latency_profile;

void latency_profile_init(latency_profile *profile);
void latency_profile_observe(latency_profile *profile,
                             uint64_t msg_age_ms,
                             uint64_t copy_ms,
                             uint64_t read_done_ms,
                             uint64_t send_ms,
                             uint64_t outq_before,
                             uint64_t outq_after);
int latency_profile_format(const latency_profile *profile,
                           char *buffer,
                           size_t buffer_size);

#endif /* LATENCY_PROFILE_H */
