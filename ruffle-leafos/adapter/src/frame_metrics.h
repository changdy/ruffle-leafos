#ifndef RUFFLE_FRAME_METRICS_H
#define RUFFLE_FRAME_METRICS_H
#include <stdint.h>
#define METRIC_BUCKETS 25
struct frame_metric {
    uint64_t sum, min, max;
    uint32_t count, buckets[METRIC_BUCKETS];
};
void metric_reset(struct frame_metric *metric);
void metric_add(struct frame_metric *metric, uint64_t microseconds);
/* Histogram upper bound (powers of two in microseconds), not exact quantile. */
uint64_t metric_percentile_bound(const struct frame_metric *metric, unsigned percentile);
#endif
