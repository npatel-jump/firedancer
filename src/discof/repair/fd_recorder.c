#include "fd_recorder.h"
#include "../../flamenco/fd_flamenco_base.h"
#include "../../util/net/fd_net_headers.h"
#include "../../flamenco/fd_rwlock.h"
#include <stdbool.h>

/* fd_recorder implementation

   IMPORTANT: All functions in this file assume that the caller has
   acquired the appropriate lock (read or write) as documented in the
   header file. This implementation does NOT acquire or release locks
   internally. 
   
  fd_recorder_new() initializes the lock structure. 
  
  fd_recorder_req_insert() requires a write lock 
  fd_recorder_req_remove() requires a write lock
  fd_recorder_req_expire() requires a write lock
  fd_recorder_verify() requires a read lock
  fd_recorder_print() requires a read lock
  fd_recorder_print_first_nonce() requires a read lock
  fd_recorder_select_peers() requires a write lock
  */

void *
fd_recorder_new( void * shmem, ulong seed, ulong timeout_ns ) {
  
  if( FD_UNLIKELY( !shmem ) ) {
    FD_LOG_WARNING(( "NULL mem" ));
    return NULL;
  }

  if( FD_UNLIKELY( !fd_ulong_is_aligned( (ulong)shmem, fd_recorder_align() ) ) ) {
    FD_LOG_WARNING(( "misaligned mem" ));
    return NULL;
  }

  ulong footprint = fd_recorder_footprint();
  if( FD_UNLIKELY( !footprint ) ) {
    FD_LOG_WARNING(( "bad footprint" ));
    return NULL;
  }

  if( FD_UNLIKELY( !timeout_ns ) ) {
    FD_LOG_WARNING(( "zero timeout_ns" ));
    return NULL;
  }

  fd_wksp_t * wksp = fd_wksp_containing( shmem );
  if( FD_UNLIKELY( !wksp ) ) {
    FD_LOG_WARNING(( "shmem must be part of a workspace" ));
    return NULL;
  }

  fd_memset( shmem, 0, footprint );

  FD_SCRATCH_ALLOC_INIT( l, shmem );
  fd_recorder_t * recorder = FD_SCRATCH_ALLOC_APPEND( l, fd_recorder_align(), sizeof( fd_recorder_t ) );
  void *       req_pool   = FD_SCRATCH_ALLOC_APPEND( l, fd_recorder_req_pool_align(), fd_recorder_req_pool_footprint( MAX_REQUESTS ) );
  void *       req_map    = FD_SCRATCH_ALLOC_APPEND( l, fd_recorder_req_map_align(), fd_recorder_req_map_footprint( fd_recorder_req_map_chain_cnt_est( MAX_REQUESTS ) ) );
  void *       req_dlist  = FD_SCRATCH_ALLOC_APPEND( l, fd_recorder_req_dlist_align(), fd_recorder_req_dlist_footprint() );
  void *       peer_pool  = FD_SCRATCH_ALLOC_APPEND( l, fd_recorder_peer_pool_align(), fd_recorder_peer_pool_footprint( FD_MAX_PEERS ) );
  void *       peer_map   = FD_SCRATCH_ALLOC_APPEND( l, fd_recorder_peer_map_align(), fd_recorder_peer_map_footprint( fd_recorder_peer_map_chain_cnt_est( FD_MAX_PEERS ) ) );

  recorder->req_pool_gaddr  = fd_wksp_gaddr_fast( wksp, fd_recorder_req_pool_join( fd_recorder_req_pool_new( req_pool, MAX_REQUESTS ) ) );
  recorder->req_map_gaddr   = fd_wksp_gaddr_fast( wksp, fd_recorder_req_map_join( fd_recorder_req_map_new( req_map, fd_recorder_req_map_chain_cnt_est( MAX_REQUESTS ), seed ) ) );
  recorder->req_dlist_gaddr = fd_wksp_gaddr_fast( wksp, fd_recorder_req_dlist_join( fd_recorder_req_dlist_new( req_dlist ) ) );
  recorder->peer_pool_gaddr = fd_wksp_gaddr_fast( wksp, fd_recorder_peer_pool_join( fd_recorder_peer_pool_new( peer_pool, FD_MAX_PEERS ) ) );
  recorder->peer_map_gaddr  = fd_wksp_gaddr_fast( wksp, fd_recorder_peer_map_join( fd_recorder_peer_map_new( peer_map, fd_recorder_peer_map_chain_cnt_est( FD_MAX_PEERS ), seed ) ) );

  recorder->recorder_gaddr = fd_wksp_gaddr_fast( wksp, recorder );
  recorder->seed              = seed;
  recorder->timeout_ns        = timeout_ns;
  recorder->req_cnt           = 0UL;
  recorder->req_expired_cnt   = 0UL;
  recorder->req_handled_cnt   = 0UL;
  recorder->peer_cnt          = 0UL;

  /* Initialize priority counts and indices */
  recorder->high_priority_cnt = 0UL;
  recorder->medium_priority_cnt = 0UL;
  recorder->low_priority_cnt = 0UL;
  recorder->zero_hr_cnt = 0UL;
  
  recorder->high_priority_idx = 0UL;
  recorder->medium_priority_idx = 0UL;
  recorder->low_priority_idx = 0UL;
  recorder->zero_hr_idx = 0UL;
  
  recorder->cycle_position = 0UL;
  recorder->cycle_count = 0UL;

  /* Initialize the read-write lock */
  recorder->rw_lock = (fd_rwlock_t){0};

  /* peer_pubkeys array removed - using map iteration instead */

  FD_COMPILER_MFENCE();
  FD_VOLATILE( recorder->magic ) = FD_RECORDER_MAGIC;
  FD_COMPILER_MFENCE();

  return shmem;
}

fd_recorder_t *
fd_recorder_join( void * shrecorder ) {
  fd_recorder_t * recorder = (fd_recorder_t *)shrecorder;

  if( FD_UNLIKELY( !recorder ) ) {
    FD_LOG_WARNING(( "NULL recorder" ));
    return NULL;
  }

  if( FD_UNLIKELY( !fd_ulong_is_aligned((ulong)recorder, fd_recorder_align() ) ) ) {
    FD_LOG_WARNING(( "misaligned recorder" ));
    return NULL;
  }

  fd_wksp_t * wksp = fd_wksp_containing( recorder );
  if( FD_UNLIKELY( !wksp ) ) {
    FD_LOG_WARNING(( "recorder must be part of a workspace" ));
    return NULL;
  }

  if( FD_UNLIKELY( recorder->magic!=FD_RECORDER_MAGIC ) ) {
    FD_LOG_WARNING(( "bad magic" ));
    return NULL;
  }

  return recorder;
}

void *
fd_recorder_leave( fd_recorder_t const * recorder ) {

  if( FD_UNLIKELY( !recorder ) ) {
    FD_LOG_WARNING(( "NULL recorder" ));
    return NULL;
  }

  return (void *)recorder;
}

void *
fd_recorder_delete( void * recorder ) {

  if( FD_UNLIKELY( !recorder ) ) {
    FD_LOG_WARNING(( "NULL recorder" ));
    return NULL;
  }

  if( FD_UNLIKELY( !fd_ulong_is_aligned((ulong)recorder, fd_recorder_align() ) ) ) {
    FD_LOG_WARNING(( "misaligned recorder" ));
    return NULL;
  }

  return recorder;
}

fd_recorder_req_t *
fd_recorder_req_insert( fd_recorder_t *             recorder,
                        ulong                       nonce,
                        ulong                       timestamp_ns,
                        fd_pubkey_t const *         pubkey,
                        fd_ip4_port_t               ip4,
                        ulong                       slot,
                        ulong                       shred_idx,
                        uint                        req_type ) {

  /* Note: Caller must hold write lock on recorder->rw_lock */

  if( FD_UNLIKELY( !recorder ) ) {
    FD_LOG_WARNING(( "NULL recorder" ));
    return NULL;
  }

  #if FD_PEER_LEDGER_USE_HANDHOLDING
  if( FD_UNLIKELY( recorder->magic != FD_RECORDER_MAGIC ) ) {
    FD_LOG_WARNING(( "bad magic" ));
    return NULL;
  }
  #endif

  fd_recorder_req_map_t * req_map  = fd_recorder_req_map( recorder );
  fd_recorder_req_t *     req_pool = fd_recorder_req_pool( recorder );

  /* Check if nonce already exists */
  #if FD_PEER_LEDGER_USE_HANDHOLDING
  if( FD_UNLIKELY( fd_recorder_req_query( recorder, nonce ) ) ) {
    FD_LOG_WARNING(( "nonce %lu already exists", nonce ));
    return NULL;
  }
  #endif

  /* Check if pool has space */
  #if FD_PEER_LEDGER_USE_HANDHOLDING
  if( FD_UNLIKELY( !fd_recorder_req_pool_free( req_pool ) ) ) {
    FD_LOG_WARNING(( "request pool full" ));
    return NULL;
  }
  #endif


  /* Allocate new request from pool */
  fd_recorder_req_t * req = fd_recorder_req_pool_ele_acquire( req_pool );
  memset( req, 0, sizeof(fd_recorder_req_t) );
  
  if( FD_UNLIKELY( !req ) ) {
    FD_LOG_WARNING(( "failed to acquire request from pool" ));
    return NULL;
  }

  fd_recorder_peer_t * peer = fd_recorder_peer_query( recorder, pubkey );
  if( FD_UNLIKELY( !peer ) ) {
    FD_LOG_WARNING(( "peer not found" ));
    return NULL;
  }
  if( FD_UNLIKELY( peer->ip4.addr != ip4.addr ) ) {
    FD_LOG_WARNING(( "IP mismatch" ));
    return NULL;
  }

  /* Initialize request fields */
  req->nonce        = nonce;
  req->timestamp_ns = timestamp_ns;
  req->pubkey       = *pubkey;
  req->slot         = slot;
  req->shred_idx    = shred_idx;
  req->req_type     = req_type;
  req->prev_idx     = ULONG_MAX;
  req->next_idx     = ULONG_MAX;

  /* Update or add peer */

  peer->num_inflight_req++;
  peer->last_send = (long)timestamp_ns;
  /* Insert into map */
  fd_recorder_req_map_ele_insert( req_map, req, req_pool );

#if FD_PEER_LEDGER_USE_HANDHOLDING
  FD_TEST( !fd_recorder_req_map_verify( req_map, fd_recorder_req_pool_max( req_pool ), req_pool ) );
#endif

  /* Get the pool index of this request */
  // ulong req_idx = fd_recorder_req_pool_idx( req_pool, req );

  /* Insert at tail of doubly linked list */
  fd_recorder_req_dlist_t * dlist = fd_recorder_req_dlist( recorder );
  fd_recorder_req_dlist_ele_push_tail( dlist, req, req_pool );

  recorder->req_cnt++;
  // FD_LOG_NOTICE(( "Request count: %lu", recorder->req_cnt ));
  // FD_LOG_INFO(("Request added: %lu, IP: "FD_IP4_ADDR_FMT ", type: %u, slot: %lu, shred_idx: %lu", req->nonce, FD_IP4_ADDR_FMT_ARGS(ip4.addr), req->req_type, req->slot, req->shred_idx));
  return req;
}

int
fd_recorder_req_remove( fd_recorder_t * recorder, ulong nonce, int is_recv ) {

  if( FD_UNLIKELY( !recorder ) ) {
    FD_LOG_WARNING(( "NULL recorder" ));
    return -1;
  }

  #if FD_PEER_LEDGER_USE_HANDHOLDING
  if( FD_UNLIKELY( recorder->magic != FD_RECORDER_MAGIC ) ) {
    FD_LOG_WARNING(( "bad magic" ));
    return -1;
  }
  #endif

  fd_recorder_req_map_t * req_map  = fd_recorder_req_map( recorder );
  fd_recorder_req_t *     req_pool = fd_recorder_req_pool( recorder );

  /* Find the request in the map */
  fd_recorder_req_t * req = fd_recorder_req_map_ele_query( req_map, &nonce, NULL, req_pool );
  if( FD_UNLIKELY( !req ) ) {
    __asm__("int $3");
    FD_LOG_WARNING(( "nonce %lu not found", nonce ));
    return -1;
  }

  fd_recorder_peer_t * peer = fd_recorder_peer_query( recorder, &req->pubkey );
  if( FD_UNLIKELY( !peer ) ) {
    FD_LOG_WARNING(( "peer not found" ));
    return -1;
  }

  fd_recorder_peer_update( recorder, &peer->key, peer->ip4, is_recv, req->timestamp_ns,  (ulong)fd_log_wallclock());

  // ulong req_idx = fd_recorder_req_pool_idx( req_pool, req );
  fd_recorder_req_dlist_t * dlist = fd_recorder_req_dlist( recorder );
  fd_recorder_req_dlist_ele_remove( dlist, req, req_pool );

  /* Remove from map */
  fd_recorder_req_map_ele_remove( req_map, &nonce, NULL, req_pool );
  fd_recorder_req_pool_ele_release( req_pool, req );

#if FD_PEER_LEDGER_USE_HANDHOLDING
  FD_TEST( !fd_recorder_req_map_verify( req_map, fd_recorder_req_pool_max( req_pool ), req_pool ) );
#endif

  if (is_recv) {
    recorder->req_handled_cnt++;
  } else {
    recorder->req_expired_cnt++;
  }

  return 0;
}

ulong
fd_recorder_req_expire( fd_recorder_t * recorder, ulong current_ns, int is_recv ) {

  if( FD_UNLIKELY( !recorder ) ) {
    FD_LOG_WARNING(( "NULL recorder" ));
    return 0UL;
  }

  #if FD_PEER_LEDGER_USE_HANDHOLDING
  if( FD_UNLIKELY( recorder->magic != FD_RECORDER_MAGIC ) ) {
    FD_LOG_WARNING(( "bad magic" ));
    return 0UL;
  }
  #endif

  fd_recorder_req_dlist_t * dlist    = fd_recorder_req_dlist( recorder );
  // fd_recorder_req_map_t *   req_map  = fd_recorder_req_map( recorder );
  fd_recorder_req_t *       req_pool = fd_recorder_req_pool( recorder );
  ulong                        expired_cnt = 0UL;

  /* Traverse from head (oldest) and remove expired requests */
  fd_recorder_req_dlist_iter_t iter = fd_recorder_req_dlist_iter_fwd_init( dlist, req_pool );
  while( !fd_recorder_req_dlist_iter_done( iter, dlist, req_pool ) ) {
    fd_recorder_req_t * req = fd_recorder_req_dlist_iter_ele( iter, dlist, req_pool );
    
    /* Check if request has expired */
    if( FD_LIKELY( req->timestamp_ns + recorder->timeout_ns > current_ns ) ) {
      /* This and all subsequent requests are not expired yet */
      break;
    }
    
    /* Advance iterator before removing */
    iter = fd_recorder_req_dlist_iter_fwd_next( iter, dlist, req_pool );
    fd_recorder_req_remove( recorder, req->nonce, is_recv );


    /* Remove expired request */

    expired_cnt++;
  }

  return expired_cnt;
}

int
fd_recorder_verify( fd_recorder_t const * recorder ) {
  if( FD_UNLIKELY( !recorder ) ) {
    FD_LOG_WARNING(( "NULL recorder" ));
    return -1;
  }

  if( FD_UNLIKELY( !fd_ulong_is_aligned( (ulong)recorder, fd_recorder_align() ) ) ) {
    FD_LOG_WARNING(( "misaligned recorder" ));
    return -1;
  }

  fd_wksp_t * wksp = fd_wksp_containing( recorder );
  if( FD_UNLIKELY( !wksp ) ) {
    FD_LOG_WARNING(( "recorder must be part of a workspace" ));
    return -1;
  }

  if( FD_UNLIKELY( recorder->magic!=FD_RECORDER_MAGIC ) ) {
    FD_LOG_WARNING(( "bad magic" ));
    return -1;
  }

  fd_recorder_req_t const *     req_pool = fd_recorder_req_pool_const( recorder );
  fd_recorder_req_map_t const * req_map  = fd_recorder_req_map_const( recorder );
  fd_recorder_req_dlist_t const * req_dlist  = fd_recorder_req_dlist_const( recorder );
  fd_recorder_peer_t const *     peer_pool = fd_recorder_peer_pool_const( recorder );
  fd_recorder_peer_map_t const * peer_map  = fd_recorder_peer_map_const( recorder );

  /* Verify map consistency */
  if( fd_recorder_req_map_verify( req_map, fd_recorder_req_pool_max( req_pool ), req_pool ) ) {
    FD_LOG_WARNING(( "map verification failed" ));
    return -1;
  }

  if( fd_recorder_peer_map_verify( peer_map, fd_recorder_peer_pool_max( peer_pool ), peer_pool ) ) {
    FD_LOG_WARNING(( "peer map verification failed" ));
    return -1;
  }

  if( fd_recorder_req_dlist_verify( req_dlist, fd_recorder_req_pool_max( req_pool ), req_pool ) ) {
    FD_LOG_WARNING(( "dlist verification failed" ));
    return -1;
  }

  return 0;
}

void
fd_recorder_print( fd_recorder_t const * recorder ) {
  if( FD_UNLIKELY( !recorder ) ) {
    FD_LOG_WARNING(( "NULL recorder" ));
    return;
  }

  FD_LOG_INFO(("Peer req_cnt: %lu, req_expired_cnt: %lu, req_handled_cnt: %lu", recorder->req_cnt, recorder->req_expired_cnt, recorder->req_handled_cnt));
}

fd_recorder_peer_t *
fd_recorder_peer_add( fd_recorder_t *             recorder,
                      fd_pubkey_t const *         pubkey,
                      fd_ip4_port_t               ip4,
                      long                        current_time ) {
  
  if( FD_UNLIKELY( !recorder || !pubkey ) ) {
    FD_LOG_WARNING(( "NULL recorder or pubkey" ));
    return NULL;
  }

  if( FD_UNLIKELY( recorder->peer_cnt >= FD_MAX_PEERS) ) {
    FD_LOG_WARNING(( "peer list full" ));
    return NULL;
  }

  fd_recorder_peer_map_t * peer_map  = fd_recorder_peer_map( recorder );
  fd_recorder_peer_t *     peer_pool = fd_recorder_peer_pool( recorder );

  /* Check if peer already exists */
  fd_recorder_peer_t * existing = fd_recorder_peer_map_ele_query( peer_map, pubkey, NULL, peer_pool );
  if( existing ) {
    /* Update existing peer info */
    // existing->ip4 = ip4;
    // existing->last_recv = current_time;
    return existing;
  }

  /* Allocate new peer */
  fd_recorder_peer_t * peer = fd_recorder_peer_pool_ele_acquire( peer_pool );
  if( FD_UNLIKELY( !peer ) ) {
    FD_LOG_WARNING(( "failed to acquire peer from pool" ));
    return NULL;
  }

  /* Initialize peer */
  peer->key               = *pubkey;
  peer->ip4               = ip4;
  peer->last_send         = 0L;
  peer->last_recv         = current_time;
  peer->ewma_hr           = -1;
  peer->ewma_rtt          = 0UL;
  peer->num_inflight_req  = 0UL;

  /* Insert into map */
  
  if( FD_UNLIKELY( !fd_recorder_peer_map_ele_insert( peer_map, peer, peer_pool ) ) ) {
    __asm__("int $3");
  }

#if FD_PEER_LEDGER_USE_HANDHOLDING
  FD_TEST( !fd_recorder_peer_map_verify( peer_map, fd_recorder_peer_pool_max( peer_pool ), peer_pool ) );
#endif

  /* Increment peer count */
  recorder->peer_cnt++;

  /* Trigger reshuffle on next select_peers call by resetting cycle */
  recorder->cycle_position = 0;
  recorder->cycle_count = 0;

  return peer;
}

// update
fd_recorder_peer_t *
fd_recorder_peer_update( fd_recorder_t *             recorder,
                         fd_pubkey_t const *         pubkey,
                         fd_ip4_port_t               ip4  FD_PARAM_UNUSED,
                         int                         is_recv,
                         ulong                       req_timestamp_ns,
                         ulong                       current_time ) {
  if( FD_UNLIKELY( !recorder || !pubkey ) ) {
    FD_LOG_WARNING(( "NULL recorder or pubkey" ));
    return NULL;
  }

  fd_recorder_peer_map_t * peer_map  = fd_recorder_peer_map( recorder );
  fd_recorder_peer_t *     peer_pool = fd_recorder_peer_pool( recorder );

  /* Check if peer already exists */
  fd_recorder_peer_t * existing = fd_recorder_peer_map_ele_query( peer_map, pubkey, NULL, peer_pool );
  if( existing ) {
    // existing->ip4 = ip4;
    // existing->last_recv = current_time;
    if (existing->ewma_hr == -1) {
      existing->ewma_hr = (double)(is_recv ? 1 : 0);
    } else if (is_recv) {
      existing->ewma_hr = (double)existing->ewma_hr * 0.9 + (double)(is_recv ? 1 : 0) * 0.1;
    }

    if (existing->ewma_rtt == 0 && is_recv) {
      existing->ewma_rtt = (double)(current_time - req_timestamp_ns);
    } else if (is_recv) {
      existing->ewma_rtt = (double)existing->ewma_rtt * 0.9 + (double)(current_time - req_timestamp_ns) * 0.1;
    }

    existing->num_inflight_req--;
    // FD_LOG_INFO(("Peer: %s, recv: %d, ewma_hr: %f, ewma_rtt: %f, num_inflight_req: %lu, sent: %lu, recv: %lu", FD_BASE58_ENC_32_ALLOCA(&existing->key), is_recv, existing->ewma_hr, existing->ewma_rtt, existing->num_inflight_req, req_timestamp_ns, current_time));
    return existing;
  }
  return NULL;
}

fd_recorder_peer_t *
fd_recorder_peer_remove( fd_recorder_t * recorder, fd_pubkey_t const * pubkey, int is_recv ) {
  fd_recorder_peer_map_t * peer_map  = fd_recorder_peer_map( recorder );
  fd_recorder_peer_t *     peer_pool = fd_recorder_peer_pool( recorder );
  fd_recorder_peer_t *     peer = fd_recorder_peer_map_ele_remove( peer_map, (void *)pubkey, NULL, peer_pool );
  if( peer ) {
#if FD_PEER_LEDGER_USE_HANDHOLDING
    FD_TEST( !fd_recorder_peer_map_verify( peer_map, fd_recorder_peer_pool_max( peer_pool ), peer_pool ) );
#endif
    recorder->peer_cnt--;

    if (is_recv) {
      recorder->req_expired_cnt++;
    } else {  
      recorder->req_handled_cnt++;
    }
  }
  return peer;
}

void
fd_recorder_peer_print( fd_recorder_peer_t * peer ) {
  FD_LOG_NOTICE(("Peer: %s, IP: "FD_IP4_ADDR_FMT", last_send: %ld, last_recv: %ld, ewma_hr: %f, ewma_rtt: %f, num_inflight_req: %lu", 
                 FD_BASE58_ENC_32_ALLOCA(&peer->key), FD_IP4_ADDR_FMT_ARGS(peer->ip4.addr), peer->last_send, peer->last_recv, peer->ewma_hr, peer->ewma_rtt, peer->num_inflight_req));
}

/* Helper function to reshuffle peers into categories based on RTT and HR */
static void
fd_recorder_reshuffle_peers( fd_recorder_t * recorder ) {
  /* Note: Caller must hold write lock on recorder->rw_lock */
  
  /* Reset category counts */
  recorder->high_priority_cnt = 0;
  recorder->medium_priority_cnt = 0;
  recorder->low_priority_cnt = 0;
  recorder->zero_hr_cnt = 0;
  
  /* Reset indices */
  recorder->high_priority_idx = 0;
  recorder->medium_priority_idx = 0;
  recorder->low_priority_idx = 0;
  recorder->zero_hr_idx = 0;
  
  fd_recorder_peer_map_t * peer_map = fd_recorder_peer_map( recorder );
  fd_recorder_peer_t * peer_pool = fd_recorder_peer_pool( recorder );

  int val = 0;
  for( fd_recorder_peer_map_iter_t iter = fd_recorder_peer_map_iter_init( peer_map, peer_pool );
  !fd_recorder_peer_map_iter_done( iter, peer_map, peer_pool );
  iter = fd_recorder_peer_map_iter_next( iter, peer_map, peer_pool ) ) {
    val++;
  }
  FD_LOG_INFO(("val: %d", val));
  
  /* Iterate through all peers in the map and categorize them */
  for( fd_recorder_peer_map_iter_t iter = fd_recorder_peer_map_iter_init( peer_map, peer_pool );
       !fd_recorder_peer_map_iter_done( iter, peer_map, peer_pool );
       iter = fd_recorder_peer_map_iter_next( iter, peer_map, peer_pool ) ) {
    
      fd_recorder_peer_t * peer = fd_recorder_peer_map_iter_ele( iter, peer_map, peer_pool );
      
      /* Check if peer has zero hit rate */
      if( peer->ewma_hr == 0.0 ) {
        recorder->zero_hr_peers[recorder->zero_hr_cnt++] = &peer->key;
      }
      else if( peer->ewma_rtt < 50000000UL ) { /* < 50ms */
        recorder->high_priority_peers[recorder->high_priority_cnt++] = &peer->key;
      }
      else if( peer->ewma_rtt < 100000000UL ) { /* 50-100ms */
        recorder->medium_priority_peers[recorder->medium_priority_cnt++] = &peer->key;
      }
      else { /* >= 100ms */
        recorder->low_priority_peers[recorder->low_priority_cnt++] = &peer->key;
      }
      
    }
  
    FD_LOG_INFO(( "Reshuffled peers - High: %lu, Medium: %lu, Low: %lu, Zero HR: %lu",
                  recorder->high_priority_cnt, recorder->medium_priority_cnt,
                  recorder->low_priority_cnt, recorder->zero_hr_cnt ));
}

void
fd_recorder_select_peers(fd_recorder_t * recorder, uint num_peers, fd_pubkey_t * selected_peers[]) {
  if( FD_UNLIKELY( !recorder || !selected_peers ) ) {
    FD_LOG_WARNING(( "NULL recorder or selected_peers" ));
    return;
  }
  
  /* Note: Caller must hold at least read lock on recorder->rw_lock */
  
  /* Check if we need to reshuffle (every 10 cycles) */
  if( FD_UNLIKELY( recorder->cycle_position == 0 && (recorder->cycle_count % 10) == 0 ) ) {
    fd_recorder_reshuffle_peers( recorder );
  }
  
  /* Define the weighted round-robin pattern:
     - High priority (<50ms): 10 polls per cycle
     - Medium priority (50-100ms): 5 polls per cycle  
     - Low priority (>=100ms): 1 poll per cycle
     - Zero HR: 1 poll per cycle (handled separately)
     
     Pattern: H,H,M,H,H,M,H,H,M,H,H,M,H,M,L,Z (16 total positions) */
  
  static const int pattern[] = {
    0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 1, 2, 3
  };
  static const ulong pattern_len = 16;
  
  uint selected_count = 0;
  
  while( selected_count < num_peers ) {
    int category = pattern[recorder->cycle_position];
    
    fd_pubkey_t * selected = NULL;
    
    switch( category ) {
      case 0: /* High priority */
        if( recorder->high_priority_cnt > 0 ) {
          selected = recorder->high_priority_peers[recorder->high_priority_idx];
          recorder->high_priority_idx = (recorder->high_priority_idx + 1) % recorder->high_priority_cnt;
        }
        break;
        
      case 1: /* Medium priority */
        if( recorder->medium_priority_cnt > 0 ) {
          selected = recorder->medium_priority_peers[recorder->medium_priority_idx];
          recorder->medium_priority_idx = (recorder->medium_priority_idx + 1) % recorder->medium_priority_cnt;
        }
        break;
        
      case 2: /* Low priority */
        if( recorder->low_priority_cnt > 0 ) {
          selected = recorder->low_priority_peers[recorder->low_priority_idx];
          recorder->low_priority_idx = (recorder->low_priority_idx + 1) % recorder->low_priority_cnt;
        }
        break;
        
      case 3: /* Zero HR */
        if( recorder->zero_hr_cnt > 0 ) {
          selected = recorder->zero_hr_peers[recorder->zero_hr_idx];
          recorder->zero_hr_idx = (recorder->zero_hr_idx + 1) % recorder->zero_hr_cnt;
        }
        break;
    }
    
    /* If we found a peer in the current category, add it */
    if( selected ) {
      selected_peers[selected_count++] = selected;
    }
    
    /* Move to next position in pattern */
    recorder->cycle_position = (recorder->cycle_position + 1) % pattern_len;
    
    /* Check if we completed a cycle */
    if( recorder->cycle_position == 0 ) {
      recorder->cycle_count++;
    }
    
    /* If no peers available in any category, break to avoid infinite loop */
    if( FD_UNLIKELY( recorder->high_priority_cnt == 0 && 
                     recorder->medium_priority_cnt == 0 && 
                     recorder->low_priority_cnt == 0 && 
                     recorder->zero_hr_cnt == 0 ) ) {
      FD_LOG_WARNING(( "No peers available for selection" ));
      break;
    }

  }
}


// HELPERS

//fd logs the first nonce in the dlist
void
fd_recorder_print_first_nonce( fd_recorder_t * recorder ) {
  fd_recorder_req_dlist_t * dlist = fd_recorder_req_dlist( recorder );
  fd_recorder_req_t * req = fd_recorder_req_dlist_iter_ele( fd_recorder_req_dlist_iter_fwd_init( dlist, fd_recorder_req_pool( recorder ) ), dlist, fd_recorder_req_pool( recorder ) );
  FD_LOG_INFO(("First nonce: %lu", req->nonce));
} 