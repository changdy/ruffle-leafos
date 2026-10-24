#include "frame_metrics.h"
void metric_reset(struct frame_metric *m) {
    m->sum = m->min = m->max = m->count = 0;
    for (unsigned i = 0; i < METRIC_BUCKETS; ++i) m->buckets[i] = 0;
}
void metric_add(struct frame_metric *m, uint64_t us) {
    if (!m->count || us < m->min) m->min = us;
    if (us > m->max) m->max = us;
    m->sum += us;
    ++m->count;
    unsigned bucket = 0;
    while (bucket < METRIC_BUCKETS - 1 && us > ((uint64_t)1 << bucket)) ++bucket;
    ++m->buckets[bucket];
}
uint64_t metric_percentile_bound(const struct frame_metric *m, unsigned percentile) {
    if (!m->count) return 0;
    if (percentile > 100) percentile = 100;
    uint32_t rank = (uint32_t)(((uint64_t)m->count * percentile + 99) / 100);
    if (!rank) rank = 1;
    uint32_t n = 0;
    for (unsigned i = 0; i < METRIC_BUCKETS; ++i) {
        n += m->buckets[i];
        if (n >= rank) return i == METRIC_BUCKETS - 1 ? m->max : ((uint64_t)1 << i);
    }
    return m->max;
}
