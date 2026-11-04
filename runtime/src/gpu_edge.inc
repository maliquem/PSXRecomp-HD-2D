/* gpu_edge.inc: edge X start and per-line step, in 32.32 fixed point. See gpu_edge.h. */
#include "gpu_edge.h"

#define GPU_EDGE_ONE ((int64_t)1 << 32)

/* Observed: every x gets the same fraction, 0xFFFFF800 (2^32 - 2^11), just under one column.
 * Multiplying (not shifting) keeps negative x well defined in C. */
static inline int64_t gpu_edge_start(int32_t x) {
    return (int64_t)x * GPU_EDGE_ONE + (GPU_EDGE_ONE - ((int64_t)1 << 11));
}

/* Observed: the size of dx/dy is rounded up, then the sign is put back, so the steps for
 * dx and -dx are exact mirrors. */
static inline int64_t gpu_edge_step(int32_t dx, int32_t dy) {
    int64_t size = (dx < 0 ? -(int64_t)dx : (int64_t)dx) * GPU_EDGE_ONE;
    int64_t step = (size + dy - 1) / dy;
    return dx < 0 ? -step : step;
}
