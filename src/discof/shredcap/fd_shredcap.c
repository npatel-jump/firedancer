#define _GNU_SOURCE 1
#include "fd_shredcap.h"
#include "../../util/fd_util.h"
#include "../../flamenco/repair/fd_repair.h"
#include "../../flamenco/types/fd_types.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

/* Helper functions for end slot bit vector tracking */

int
fd_shredcap_end_slot_shred_test( fd_repair_test_t * ctx, uint shred_idx ) {
  ulong word_idx = shred_idx / 64UL;
  ulong bit_idx  = shred_idx % 64UL;
  return !!(ctx->end_slot_shred_bits[word_idx] & (1UL << bit_idx));
}

void
fd_shredcap_end_slot_shred_insert( fd_repair_test_t * ctx, uint shred_idx ) {
  ulong word_idx = shred_idx / 64UL;
  ulong bit_idx  = shred_idx % 64UL;
  ctx->end_slot_shred_bits[word_idx] |= (1UL << bit_idx);
}

void
fd_shredcap_end_slot_reset_tracking( fd_repair_test_t * ctx ) {
  ctx->end_slot_buffered_idx = 0U;
  ctx->end_slot_complete_idx = UINT_MAX;
  ctx->finished_timer = 0;  /* Reset the finished timer */
  memset( ctx->end_slot_shred_bits, 0, sizeof(ctx->end_slot_shred_bits) );
}

/* Slot timing tracking functions */

void
fd_shredcap_slot_timing_init( fd_repair_test_t * ctx ) {
  ctx->slot_count_tracked = 0U;
  memset( ctx->slot_timing, 0, sizeof(ctx->slot_timing) );
}

void
fd_shredcap_slot_timing_request( fd_repair_test_t * ctx, ulong slot, uint request_type ) {
  /* Only track non-orphan requests */
  if( request_type == fd_repair_protocol_enum_orphan ) {
    return;
  }

  /* Calculate array index relative to start_slot */
  if( slot < ctx->start_slot ) {
    return;
  }

  ulong slot_idx = slot - ctx->start_slot;
  if( slot_idx >= FD_REPAIR_TEST_MAX_SLOTS ) {
    return;
  }

  /* If this is the first regular request for this slot, set start time and flag */
  if( !ctx->slot_timing[slot_idx].regular_requests_sent ) {
    ctx->slot_timing[slot_idx].start_time = fd_log_wallclock();
    ctx->slot_timing[slot_idx].regular_requests_sent = 1;
    ctx->slot_count_tracked++;
  }
}

void
fd_shredcap_slot_timing_response( fd_repair_test_t * ctx, ulong slot, long timestamp ) {
  /* Calculate array index relative to start_slot */
  if( slot < ctx->start_slot ) {
    return;
  }

  ulong slot_idx = slot - ctx->start_slot;
  if( slot_idx >= FD_REPAIR_TEST_MAX_SLOTS ) {
    return;
  }

  /* Only track end time if we've sent regular requests for this slot */
  if( ctx->slot_timing[slot_idx].regular_requests_sent ) {
    ctx->slot_timing[slot_idx].end_time = timestamp;
  }
}

double
fd_shredcap_slot_timing_average( fd_repair_test_t * ctx ) {
  if( ctx->slot_count_tracked == 0U ) {
    return 0.0;
  }

  long total_duration_ns = 0L;
  uint completed_slots = 0U;

  /* Iterate through all tracked slots */
  for( uint i = 0U; i < FD_REPAIR_TEST_MAX_SLOTS; i++ ) {
    /* Only count slots where we have both start and end times */
    if( ctx->slot_timing[i].regular_requests_sent &&
        ctx->slot_timing[i].start_time > 0L &&
        ctx->slot_timing[i].end_time > 0L ) {

      long duration = ctx->slot_timing[i].end_time - ctx->slot_timing[i].start_time;
      if( duration > 0L ) {
        total_duration_ns += duration;
        completed_slots++;
      }
    }
  }

  if( completed_slots == 0U ) {
    return 0.0;
  }

  /* ns -> s */
  double avg_duration_ms = (double)total_duration_ns / (double)completed_slots / 1000000.0;
  return avg_duration_ms;
}

/* Clock functions for timer display */

void
fd_shredcap_timer_move_to_bottom_and_display( fd_repair_test_t * ctx ) {
    /* Show normal timer with elapsed time */
  printf("\n\033[35mRepair Range: [%lu, %lu] | Elapsed: %.2fs | Total Repair Requests: %u | Total Received Responses: %u\033[0m",
          ctx->start_slot, ctx->end_slot,
          (double)(fd_log_wallclock() - ctx->repair_start_time) / 1e9,
          ctx->total_repair_requests,
          ctx->total_received_shreds);

  printf("\033[1A\r");
  fflush(stdout);
}

void
fd_shredcap_timer_clear_bottom_line( void ) {
  printf("\r\033[K");
  fflush(stdout);
}


void
fd_shredcap_repair_test_init( fd_repair_test_t * ctx ) {
  ctx->total_received_shreds = 0;
  ctx->total_repair_requests = 0;
  ctx->first_shred_slot = ULONG_MAX;
  ctx->last_shred_slot = 0;

  ctx->repair_start_time = 0;
  ctx->last_timer_update = 0;
  ctx->timer_line_active = 0;
  ctx->repair_timer_started = 0;
  ctx->finished_timer = 0;

  fd_shredcap_slot_timing_init( ctx );

  ctx->total_rtt_ns = 0L;
  ctx->rtt_measurement_count = 0UL;
}

void
fd_shredcap_repair_timer_start( fd_repair_test_t * ctx ) {
  if( !ctx->repair_timer_started ) {
    ctx->repair_start_time = fd_log_wallclock();
    ctx->last_timer_update = ctx->repair_start_time;
    ctx->repair_timer_started = 1;
    printf( "\033[35mREPAIR_TIMER_START: First repair request detected, timer started for end_slot %lu\033[0m\n", ctx->end_slot );
  }
}

void
fd_shredcap_repair_request_count( fd_repair_test_t * ctx, ulong slot, uint discriminant, uint nonce FD_PARAM_UNUSED, uint shred_idx FD_PARAM_UNUSED ) {
  ctx->total_repair_requests++;
  switch( discriminant ) {
    case fd_repair_protocol_enum_window_index:
      ctx->total_window_index_requests++;
      break;
    case fd_repair_protocol_enum_highest_window_index:
      ctx->total_highest_window_index_requests++;
      break;
    case fd_repair_protocol_enum_orphan:
      ctx->total_orphan_requests++;
      break;
  }

  /* Track slot timing for non-orphan requests */
  fd_shredcap_slot_timing_request( ctx, slot, discriminant );
  fd_shredcap_rtt_track_outgoing_request( ctx, nonce, slot, shred_idx, discriminant, fd_log_wallclock() );

}

int
fd_shredcap_repair_test_process_shred( fd_repair_test_t * ctx, ulong slot, uint idx, int is_data, fd_shred_t const * shred, uint nonce FD_PARAM_UNUSED ) {
  if( ctx->first_shred_slot == ULONG_MAX ) ctx->first_shred_slot = slot;
  if( slot > ctx->last_shred_slot ) ctx->last_shred_slot = slot;

  if( ctx->repair_timer_started ) {
    ctx->total_received_shreds++;
  }

  /* Track slot timing for this shred */
  long now = fd_log_wallclock();
  fd_shredcap_slot_timing_response( ctx, slot, now );

  /* Update running timer display every 0.05 seconds */
  if( !ctx->repair_timer_started ) {
    if( ctx->last_timer_update == 0 || now - ctx->last_timer_update > 1000000000L ) { /* 1 second */
      fd_shredcap_timer_move_to_bottom_and_display( ctx );
      ctx->last_timer_update = now;
    }
  } else {
    if( now - ctx->last_timer_update > 100000000L ) { /* 0.1 seconds */
      fd_shredcap_timer_move_to_bottom_and_display( ctx );
      ctx->last_timer_update = now;
    }
  }
  if (nonce != 0) {
  fd_shredcap_rtt_track_incoming_response( ctx, nonce, slot, idx, fd_log_wallclock() );
  }

  if( slot == ctx->end_slot ) {
    fd_shredcap_end_slot_shred_insert( ctx, idx );

    while( fd_shredcap_end_slot_shred_test( ctx, ctx->end_slot_buffered_idx + 1U ) ) {
      ctx->end_slot_buffered_idx++;
    }

    if( is_data ) {
      fd_shred_t const * data_shred = shred;
      int slot_complete = !!(data_shred->data.flags & FD_SHRED_DATA_FLAG_SLOT_COMPLETE);
      if( slot_complete ) {
        ctx->end_slot_complete_idx = idx;
        printf( "\033[35mEnd slot %lu marked complete at shred idx %u \033[0m\n", slot, idx );
        fd_shredcap_repair_test_complete( ctx );
        return 0;
      }
    }
    fd_shredcap_timer_move_to_bottom_and_display( ctx );
  }

  return 0;
}

void
fd_shredcap_repair_test_complete( fd_repair_test_t * ctx ) {
  /* Clear the timer line and flush output properly */
  fd_shredcap_timer_clear_bottom_line();
  printf("\n");
  fflush(stdout);

  ulong request_response_diff = ctx->total_repair_requests - ctx->total_received_shreds;

  long completion_time = fd_log_wallclock();
  double elapsed_sec = ctx->repair_timer_started ? (double)(completion_time - ctx->repair_start_time) / 1e9 : 0.0;

  ulong slots_repaired = ctx->end_slot - ctx->start_slot + 1;
  double ms_per_slot = (elapsed_sec > 0.0 && slots_repaired > 0) ? (elapsed_sec * 1000.0) / (double)slots_repaired : 0.0;

  /* Calculate average slot completion time */
  double avg_slot_completion_ms = fd_shredcap_slot_timing_average( ctx );

  /* Display statistics page */
  printf("\n\n");
  printf("\033[35m╔═══════════════════════════════════════════════════════════════╗\033[0m\n");
  printf("\033[35m║\033[0m                     REPAIR TEST STATISTICS                    \033[35m║\033[0m\n");
  printf("\033[35m╠═══════════════════════════════════════════════════════════════╣\033[0m\n");
  printf("\033[35m║\033[0m   Start Slot:                 %12lu                    \033[35m║\033[0m\n", ctx->start_slot);
  printf("\033[35m║\033[0m   End Slot:                   %12lu                    \033[35m║\033[0m\n", ctx->end_slot);
  printf("\033[35m║\033[0m   Slots Repaired:             %12u                    \033[35m║\033[0m\n", ctx->slot_count_tracked);
  printf("\033[35m║\033[0m   Total Repair Requests:      %12u                    \033[35m║\033[0m\n", ctx->total_repair_requests);
  printf("\033[35m║\033[0m       Orphan:                 %12u                    \033[35m║\033[0m\n", ctx->total_orphan_requests);
  printf("\033[35m║\033[0m       Window Index:           %12u                    \033[35m║\033[0m\n", ctx->total_window_index_requests);
  printf("\033[35m║\033[0m       Highest Window Index:   %12u                    \033[35m║\033[0m\n", ctx->total_highest_window_index_requests);
  printf("\033[35m║\033[0m   Total Received Responses:   %12u                    \033[35m║\033[0m\n", ctx->total_received_shreds);
  printf("\033[35m║\033[0m  Unmatched Repair Requests:   %12lu                    \033[35m║\033[0m\n", request_response_diff);
  printf("\033[35m║\033[0m                                                               \033[35m║\033[0m\n");
  printf("\033[35m║\033[0m                                                               \033[35m║\033[0m\n");
  if( ctx->repair_timer_started ) {
    printf("\033[35m║\033[0m   Total Repair Time:          %12.3f seconds            \033[35m║\033[0m\n", elapsed_sec);
    printf("\033[35m║\033[0m   Milliseconds per Slot:      %12.2f ms                 \033[35m║\033[0m\n", ms_per_slot);
  } else {
    printf("\033[35m║\033[0m  No repair timing available (no repair requests detected)         \033[35m║\033[0m\n");
  }
  if( avg_slot_completion_ms > 0.0 ) {
    printf("\033[35m║\033[0m   Avg Slot Completion Time:   %12.3f ms                 \033[35m║\033[0m\n", avg_slot_completion_ms);
  } else {
    printf("\033[35m║\033[0m  No slot completion timing available                              \033[35m║\033[0m\n");
  }

  /* Calculate and display mean RTT */
  if( ctx->rtt_measurement_count > 0UL ) {
    double mean_rtt_ms = (double)ctx->total_rtt_ns / (double)ctx->rtt_measurement_count / 1000000.0;
    printf("\033[35m║\033[0m   Mean RTT:                   %12.3f ms                 \033[35m║\033[0m\n", mean_rtt_ms);
    printf("\033[35m║\033[0m   RTT Measurements Count:     %12lu                    \033[35m║\033[0m\n", ctx->rtt_measurement_count);
  } else {
    printf("\033[35m║\033[0m  No RTT measurements available                                     \033[35m║\033[0m\n");
  }

  printf("\033[35m╚═══════════════════════════════════════════════════════════════╝\033[0m\n");
  printf("\n");
  fflush(stdout);

  printf( "\033[35mRepair test completed successfully. Shutting down...\033[0m\n" );
  fflush(stdout);
  fflush(stderr);

  exit(0);
}

/* RTT tracking helper functions for shredcap tile */

void
fd_shredcap_rtt_track_outgoing_request( fd_repair_test_t * ctx,
                                         uint nonce,
                                         ulong slot,
                                         uint shred_idx,
                                         uint request_type,
                                         long timestamp ) {
  if( !ctx || !ctx->inflight_map || !ctx->inflight_pool ) return;

  fd_rtt_inflight_request_t * entry = fd_rtt_inflight_pool_ele_acquire( ctx->inflight_pool );
  if( !entry ) {
    return;
  }

  entry->nonce          = nonce;
  entry->start_time     = timestamp;
  entry->end_time       = 0L;
  entry->slot           = slot;
  entry->shred_idx      = shred_idx;
  entry->request_type   = request_type;
  entry->slot_shred_key = (slot << 32) | (ulong)shred_idx;

  (void)fd_rtt_inflight_map_ele_insert( ctx->inflight_map, entry, ctx->inflight_pool );
}

void
fd_shredcap_rtt_track_incoming_response( fd_repair_test_t * ctx,
                                          uint nonce,
                                          ulong slot,
                                          uint shred_idx,
                                          long timestamp ) {
  if( !ctx || !ctx->inflight_map || !ctx->measurement_map || !ctx->inflight_pool || !ctx->measurement_pool ) return;

  fd_rtt_inflight_request_t * req = fd_rtt_inflight_map_ele_query( ctx->inflight_map, &nonce, NULL, ctx->inflight_pool );
  if( !req ) {
    return;
  }

  /* Update the end time and find the key which is determined via a slot
  and shred index. This key works as long the repair tool is not used
  over a sized slot, or number of slots that is larger than 2^32 (which
  I do not think is possible). */
  req->end_time = timestamp;
  ulong slot_shred_key = (slot << 32) | (ulong)shred_idx;

  fd_rtt_inflight_request_t * existing = fd_rtt_measurement_map_ele_query( ctx->measurement_map, &slot_shred_key, NULL, ctx->measurement_pool );

  if( !existing ) {
    fd_rtt_inflight_request_t * measurement_entry = fd_rtt_measurement_pool_ele_acquire( ctx->measurement_pool );
    if( measurement_entry ) {
      *measurement_entry = *req;
      measurement_entry->slot_shred_key = slot_shred_key;
      (void)fd_rtt_measurement_map_ele_insert( ctx->measurement_map, measurement_entry, ctx->measurement_pool );

      /* Update RTT statistics */
      long rtt_ns = req->end_time - req->start_time;
      if( rtt_ns > 0L ) {
        ctx->total_rtt_ns += rtt_ns;
        ctx->rtt_measurement_count++;
      }
    }
  }
  /* If already exists in measurement map, we just ignore this duplicate */

  (void)fd_rtt_inflight_map_ele_remove( ctx->inflight_map, &nonce, NULL, ctx->inflight_pool );
  fd_rtt_inflight_pool_ele_release( ctx->inflight_pool, req );
}


