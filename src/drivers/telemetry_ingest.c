#include <string.h>

#include "drivers/telemetry_ingest.h"

#define WRITE_SLOT(cursor, ptr) memcpy((cursor), (ptr), sizeof(TelemetryFrame))

static void cbuf_insert(AppState *state, const TelemetryFrame *frame) {
    size_t idx = state->telemetry_frames % QUEUE_SIZE;
    memcpy(&state->shared.queue[idx], frame, sizeof(TelemetryFrame));
    state->telemetry_frames++;
    state->telemetry_cursor = &state->shared.queue[state->telemetry_frames % QUEUE_SIZE];
}

static void ring_buffer_commit(AppState *state, const TelemetryFrame *frame) {
    cbuf_insert(state, frame);
}

static void dma_descriptor_stage(AppState *state, const TelemetryFrame *frame) {
    ring_buffer_commit(state, frame);
}

void fsw_tm_push(AppState *state, const TelemetryFrame *frame) {
    dma_descriptor_stage(state, frame);
}
