/* gpu_edge.h: the X of a triangle's edges, line by line. Interface only.
 *
 * Implemented in gpu_edge.inc, a clean-room rewrite: 0 differences in 33 million cases against the
 * code it replaced (method in docs/SPU_ENV_CLEAN_ROOM.md).
 *
 * The GPU fills a triangle one horizontal line at a time. Along each edge it keeps X as a signed
 * 64-bit fixed-point number with 32 fraction bits, and adds a constant step per line. The pixel column
 * is the value shifted right by 32. The vertices are sorted by Y first, so dy is always above 0.
 */
#ifndef GPU_EDGE_H
#define GPU_EDGE_H
#include <stdint.h>

/* The fixed-point X at which an edge starts, from a vertex's integer X. */
static inline int64_t gpu_edge_start(int32_t x);

/* The step per line of an edge going dx columns across dy lines (dy >= 1). */
static inline int64_t gpu_edge_step(int32_t dx, int32_t dy);

#endif
