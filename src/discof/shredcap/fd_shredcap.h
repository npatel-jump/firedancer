#ifndef HEADER_fd_src_discof_shredcap_fd_shredcap_h
#define HEADER_fd_src_discof_shredcap_fd_shredcap_h

#include "../../disco/fd_disco_base.h"
#include "../../flamenco/types/fd_types.h"
#include <string.h>

/* Inflight request tracking for RTT measurement */
struct fd_rtt_inflight_request {
  uint  nonce;         /* Request nonce (key) */
  ulong next;
  long  start_time;
  long  end_time;
  ulong slot;
  uint  shred_idx;
  uint  request_type;
  ulong slot_shred_key; /* Packed key: (slot << 32) | shred_idx */
};
typedef struct fd_rtt_inflight_request fd_rtt_inflight_request_t;

/* Pool and map definitions for inflight requests */
#define POOL_NAME     fd_rtt_inflight_pool
#define POOL_T        fd_rtt_inflight_request_t
#include "../../util/tmpl/fd_pool.c"

#define MAP_NAME      fd_rtt_inflight_map
#define MAP_ELE_T     fd_rtt_inflight_request_t
#define MAP_KEY_T     uint
#define MAP_KEY       nonce
#include "../../util/tmpl/fd_map_chain.c"

/* Pool and map definitions for RTT measurements */
#define POOL_NAME     fd_rtt_measurement_pool
#define POOL_T        fd_rtt_inflight_request_t
#include "../../util/tmpl/fd_pool.c"

#define MAP_NAME      fd_rtt_measurement_map
#define MAP_ELE_T     fd_rtt_inflight_request_t
#define MAP_KEY_T     ulong
#define MAP_KEY       slot_shred_key
#include "../../util/tmpl/fd_map_chain.c"


/* Slot timing entry */
struct fd_slot_timing {
  long start_time;           /* Time when first non-orphan request was sent */
  long end_time;             /* Time when response shred was received */
  int  regular_requests_sent; /* Flag: have we sent non-orphan requests for this slot */
};
typedef struct fd_slot_timing fd_slot_timing_t;

/* Repair test context - only contains fields relevant to repair testing */
struct fd_repair_test {
  ulong start_slot;
  ulong end_slot;

  /* Fields for repair_test end slot tracking */
  ulong * shred_count_fseq;  /* Total expected shreds in end slot */
  uint    end_slot_buffered_idx; /* Highest contiguous shred received */
  uint    end_slot_complete_idx; /* Last shred index when slot is complete */
  ulong   end_slot_shred_bits[64]; /* Bit vector tracking which shreds received (supports up to 4096 shreds) */

  long    finished_timer;        /* When the end slot was marked complete */

  /* Slot timing tracking - fixed size arrays for no malloc */
  #define FD_REPAIR_TEST_MAX_SLOTS 10000

  fd_slot_timing_t slot_timing[FD_REPAIR_TEST_MAX_SLOTS]; /* Per-slot timing data */
  uint    slot_count_tracked; /* Number of unique slots we've seen */

  /* Running timer state */
  long    repair_start_time;     /* When repair test started */
  long    last_timer_update;     /* Last time we updated the timer display */
  int     timer_line_active;     /* Whether we have an active timer line */
  int     repair_timer_started;  /* Whether we've seen the first repair request */

  /* Repair statistics */
  uint    total_received_shreds; /* Count of all incoming shreds during repair */
  uint    total_repair_requests; /* Count of repair requests sent */
  uint    total_window_index_requests; /* Count of window index repair requests sent */
  uint    total_highest_window_index_requests; /* Count of highest window index repair requests sent */
  uint    total_orphan_requests; /* Count of orphan repair requests sent */
  ulong   first_shred_slot;      /* First slot we saw a shred for */
  ulong   last_shred_slot;       /* Last slot we saw a shred for */

  /* RTT tracking fields */
  fd_rtt_inflight_request_t * inflight_pool;      /* Pool for inflight requests */
  fd_rtt_inflight_map_t *     inflight_map;       /* Map for tracking inflight requests by nonce */
  fd_rtt_inflight_request_t * measurement_pool;   /* Pool for RTT measurements */
  fd_rtt_measurement_map_t *  measurement_map;    /* Map for completed measurements by slot_shred_key */

  /* RTT statistics */
  long  total_rtt_ns;           /* Running sum of all RTT measurements in nanoseconds */
  ulong rtt_measurement_count;  /* Count of shreds added to measurement map */
};
typedef struct fd_repair_test fd_repair_test_t;

FD_PROTOTYPES_BEGIN

/* Clock functions for timer display */
void fd_shredcap_timer_move_to_bottom_and_display( fd_repair_test_t * ctx );
void fd_shredcap_timer_clear_bottom_line( void );

/* Afterfrag functions for repair_test logic */
void fd_shredcap_repair_test_init( fd_repair_test_t * ctx );
void fd_shredcap_repair_timer_start( fd_repair_test_t * ctx );
void fd_shredcap_repair_request_count( fd_repair_test_t * ctx, ulong slot, uint discriminant, uint nonce, uint shred_idx );
int  fd_shredcap_repair_test_process_shred( fd_repair_test_t * ctx, ulong slot, uint idx, int is_data, fd_shred_t const * shred, uint nonce );
void fd_shredcap_repair_test_complete( fd_repair_test_t * ctx );

/* Helper functions for end_slot bit vector tracking */
int  fd_shredcap_end_slot_shred_test( fd_repair_test_t * ctx, uint shred_idx );
void fd_shredcap_end_slot_shred_insert( fd_repair_test_t * ctx, uint shred_idx );
void fd_shredcap_end_slot_reset_tracking( fd_repair_test_t * ctx );

/* Helper functions for slot timing tracking */
void fd_shredcap_slot_timing_init( fd_repair_test_t * ctx );
void fd_shredcap_slot_timing_update( fd_repair_test_t * ctx, ulong slot, long timestamp );
double fd_shredcap_slot_timing_average( fd_repair_test_t * ctx );

/* New slot timing functions for request/response tracking */
void fd_shredcap_slot_timing_request( fd_repair_test_t * ctx, ulong slot, uint request_type );
void fd_shredcap_slot_timing_response( fd_repair_test_t * ctx, ulong slot, long timestamp );

/* RTT tracking functions */
void fd_shredcap_rtt_tracker_init( fd_repair_test_t * ctx, uchar * mem );
void fd_shredcap_rtt_tracker_fini( fd_repair_test_t * ctx );
void fd_shredcap_rtt_track_request( fd_repair_test_t * ctx, uint nonce, ulong slot, uint shred_idx, uint request_type, long timestamp );
void fd_shredcap_rtt_track_response( fd_repair_test_t * ctx, uint nonce, ulong slot, uint shred_idx, long timestamp );
double fd_shredcap_rtt_calculate_mean( fd_repair_test_t * ctx );
ulong fd_shredcap_rtt_footprint( void );
ulong fd_shredcap_rtt_align( void );

/* Helper functions for protocol message processing */
void fd_shredcap_rtt_track_repair_request( fd_repair_test_t * ctx, uchar const * repair_buffer, ulong buffer_sz );
void fd_shredcap_rtt_track_shred_response( fd_repair_test_t * ctx, ulong slot, uint shred_idx );

/* RTT tracking helper functions for shredcap tile */
void fd_shredcap_rtt_track_outgoing_request( fd_repair_test_t * ctx, uint nonce, ulong slot, uint shred_idx, uint request_type, long timestamp );
void fd_shredcap_rtt_track_incoming_response( fd_repair_test_t * ctx, uint nonce, ulong slot, uint shred_idx, long timestamp );

FD_PROTOTYPES_END

#endif /* HEADER_fd_src_discof_shredcap_fd_shredcap_h */
