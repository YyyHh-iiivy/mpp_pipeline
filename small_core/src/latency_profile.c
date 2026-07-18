#include "latency_profile.h"

#include <stdio.h>
#include <string.h>

static void metric_observe(latency_profile_metric *metric, uint64_t value)
{
    if (metric->count < LATENCY_PROFILE_WINDOW) {
        metric->samples[metric->count++] = value;
    } else {
        metric->samples[metric->next] = value;
        metric->next = (metric->next + 1U) % LATENCY_PROFILE_WINDOW;
    }

    metric->total++;
    metric->sum += value;
    if (value > metric->max)
        metric->max = value;
}

static uint64_t metric_p90(const latency_profile_metric *metric)
{
    uint64_t sorted[LATENCY_PROFILE_WINDOW];
    size_t i;
    size_t j;
    size_t index;

    if (metric->count == 0)
        return 0;

    memcpy(sorted, metric->samples, metric->count * sizeof(sorted[0]));
    for (i = 1; i < metric->count; i++) {
        uint64_t value = sorted[i];
        j = i;
        while (j > 0 && sorted[j - 1] > value) {
            sorted[j] = sorted[j - 1];
            j--;
        }
        sorted[j] = value;
    }

    index = (metric->count * 9U + 9U) / 10U;
    if (index == 0)
        index = 1;
    if (index > metric->count)
        index = metric->count;
    return sorted[index - 1U];
}

static uint64_t metric_avg(const latency_profile_metric *metric)
{
    if (metric->total == 0)
        return 0;
    return metric->sum / metric->total;
}

static int append_metric(char *buffer,
                         size_t buffer_size,
                         size_t *used,
                         const char *avg_name,
                         const char *p90_name,
                         const char *max_name,
                         const latency_profile_metric *metric)
{
    int written;

    written = snprintf(buffer + *used,
                       buffer_size - *used,
                       " %s=%llu %s=%llu %s=%llu",
                       avg_name,
                       (unsigned long long)metric_avg(metric),
                       p90_name,
                       (unsigned long long)metric_p90(metric),
                       max_name,
                       (unsigned long long)metric->max);
    if (written < 0 || (size_t)written >= buffer_size - *used)
        return -1;
    *used += (size_t)written;
    return 0;
}

void latency_profile_init(latency_profile *profile)
{
    if (profile)
        memset(profile, 0, sizeof(*profile));
}

void latency_profile_observe(latency_profile *profile,
                             uint64_t msg_age_ms,
                             uint64_t copy_ms,
                             uint64_t read_done_ms,
                             uint64_t send_ms,
                             uint64_t outq_before,
                             uint64_t outq_after)
{
    if (!profile)
        return;
    metric_observe(&profile->msg_age_ms, msg_age_ms);
    metric_observe(&profile->copy_ms, copy_ms);
    metric_observe(&profile->read_done_ms, read_done_ms);
    metric_observe(&profile->send_ms, send_ms);
    metric_observe(&profile->outq_before, outq_before);
    metric_observe(&profile->outq_after, outq_after);
}

int latency_profile_format(const latency_profile *profile,
                           char *buffer,
                           size_t buffer_size)
{
    size_t used = 0;
    int written;

    if (!profile || !buffer || buffer_size == 0 || profile->msg_age_ms.total == 0)
        return 0;

    written = snprintf(buffer, buffer_size, "count=%llu",
                       (unsigned long long)profile->msg_age_ms.total);
    if (written < 0 || (size_t)written >= buffer_size)
        return -1;
    used = (size_t)written;

    if (append_metric(buffer, buffer_size, &used,
                      "msg_age_avg_ms", "msg_age_p90_ms", "msg_age_max_ms",
                      &profile->msg_age_ms) != 0 ||
        append_metric(buffer, buffer_size, &used,
                      "copy_avg_ms", "copy_p90_ms", "copy_max_ms",
                      &profile->copy_ms) != 0 ||
        append_metric(buffer, buffer_size, &used,
                      "read_done_avg_ms", "read_done_p90_ms", "read_done_max_ms",
                      &profile->read_done_ms) != 0 ||
        append_metric(buffer, buffer_size, &used,
                      "send_avg_ms", "send_p90_ms", "send_max_ms",
                      &profile->send_ms) != 0 ||
        append_metric(buffer, buffer_size, &used,
                      "outq_before_avg", "outq_before_p90", "outq_before_max",
                      &profile->outq_before) != 0 ||
        append_metric(buffer, buffer_size, &used,
                      "outq_after_avg", "outq_after_p90", "outq_after_max",
                      &profile->outq_after) != 0)
        return -1;
    return (int)used;
}
