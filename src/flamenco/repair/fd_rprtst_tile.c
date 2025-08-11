#include "../../disco/tiles.h"
#include "./generated/fd_rprtst_tile_seccomp.h"

#include "../../tango/mcache/fd_mcache.h"
#include "../../util/bits/fd_bits.h"

#include <linux/unistd.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>

/* Basic context structure for repair test tile */
struct fd_rprtst_ctx {
  /* Basic tile state */
  fd_wksp_t * wksp;

  /* Flag to track if this is the first credit cycle */
  int first_credit_cycle;

  /* Output link for gossip_repai */
  fd_frag_meta_t * gossip_repair_out_mcache;
  ulong *          gossip_repair_out_sync;
  ulong            gossip_repair_out_depth;
  ulong            gossip_repair_out_seq;

  fd_wksp_t * gossip_repair_out_mem;
  ulong       gossip_repair_out_chunk0;
  ulong       gossip_repair_out_wmark;
  ulong       gossip_repair_out_chunk;
};
typedef struct fd_rprtst_ctx fd_rprtst_ctx_t;

/* Gossip logic functions moved from repair_test.c */
static ulong
gossip_intake( void ) {
  // Construct path to Python helper script and run it to generate/update repair peers data
  char script_path[PATH_MAX];
  char repair_peers_path[PATH_MAX];
  char *file_dir = strdup(__FILE__);
  char *last_slash = strrchr(file_dir, '/');
  if (last_slash) {
    *last_slash = '\0';
    snprintf(script_path, sizeof(script_path), "%s/../../app/firedancer-dev/commands/repair_test_helper.py", file_dir);
    snprintf(repair_peers_path, sizeof(repair_peers_path), "%s/../../app/firedancer-dev/commands/repair_peers.bin", file_dir);
  } else {
    strcpy(script_path, "repair_test_helper.py");
    strcpy(repair_peers_path, "repair_peers.bin");
  }
  free(file_dir);

  // Delete existing binary file to ensure fresh data
  if (access(repair_peers_path, F_OK) == 0) {
    if (unlink(repair_peers_path) == 0) {
      FD_LOG_NOTICE(("Deleted existing repair_peers.bin"));
    } else {
      FD_LOG_ERR(("No existing repair_peers.bin to delete (or deletion failed)"));
    }
  }

  // Run Python script to fetch and generate repair peers data
  char python_cmd[PATH_MAX + 20];
  snprintf(python_cmd, sizeof(python_cmd), "python3 %s", script_path);
  FD_LOG_INFO(("Running: %s", python_cmd));
  int ret = system(python_cmd);
  if (ret != 0) {
    FD_LOG_WARNING(("Python script failed with return code: %d", ret));
  }

  FILE *file = fopen(repair_peers_path, "rb");
  if (file) {
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    FD_LOG_INFO(("repair_peers.bin file size: %ld bytes", file_size));

    // Each peer entry is 38 bytes: 32 pubkey, 4 ip, 2 port
    const size_t entry_size = 38;
    uchar buf[entry_size];
    size_t idx = 0;
    while (fread(buf, 1, entry_size, file) == entry_size) {
      FD_LOG_HEXDUMP_INFO(("repair_peer[%zu]", buf, entry_size));
      idx++;
    }
    FD_LOG_INFO(("Loaded %zu repair peers", idx));
    fclose(file);
    return idx;
  } else {
    FD_LOG_WARNING(("Could not open %s for reading", repair_peers_path));
    return 0UL;
  }
}

static void
forge_gossip_messages( fd_rprtst_ctx_t * ctx ) {
  // Construct path to repair peers binary file
  char repair_peers_path[PATH_MAX];
  char *file_dir = strdup(__FILE__);
  char *last_slash = strrchr(file_dir, '/');
  if (last_slash) {
    *last_slash = '\0';
    snprintf(repair_peers_path, sizeof(repair_peers_path), "%s/../../app/firedancer-dev/commands/repair_peers.bin", file_dir);
  } else {
    strcpy(repair_peers_path, "repair_peers.bin");
  }
  free(file_dir);

  FILE *file = fopen(repair_peers_path, "rb");
  if (file) {
     // Get file size to determine total peers
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    const long entry_size = 38;
    ulong peer_count = (ulong)(file_size / entry_size);

    if (peer_count > 0) {
      void * chunk_laddr = fd_chunk_to_laddr( ctx->gossip_repair_out_mem, ctx->gossip_repair_out_chunk );

      // Read all peer data into the chunk
      long bytes_read = (long)fread( chunk_laddr, 1, (ulong)file_size, file ); // maybe a firedancer function here? did not get chance to check therfore used fread

      if (bytes_read == file_size) {
        ulong sig = 0UL;
        ulong ctl = fd_frag_meta_ctl( 0UL, 1, 1, 0 );
        ulong ts = (ulong)fd_frag_meta_ts_comp( fd_tickcount() );

        // Publish single message with peer_count as the data size (used by repair so that ushort field size doesnt truncate)
        fd_mcache_publish( ctx->gossip_repair_out_mcache,
                           ctx->gossip_repair_out_depth,
                           ctx->gossip_repair_out_seq, sig,
                           ctx->gossip_repair_out_chunk,
                           peer_count, ctl, ts, ts );

        ctx->gossip_repair_out_chunk = fd_dcache_compact_next( ctx->gossip_repair_out_chunk,
                                                               (ulong)file_size,
                                                               ctx->gossip_repair_out_chunk0,
                                                               ctx->gossip_repair_out_wmark );

        FD_LOG_INFO(("Published %lu gossip peers in single message (total bytes: %lu)", peer_count, (ulong)file_size));
      } else {
        FD_LOG_WARNING(("Failed to read complete peer data: got %lu bytes, expected %lu", (ulong)bytes_read, (ulong)file_size));
      }
    } else {
      FD_LOG_WARNING(("No peers found in file"));
    }

    fclose(file);
  } else {
    FD_LOG_WARNING(("Could not open %s for forging gossip messages", repair_peers_path));
  }
}

/* Implementation of after_frag callback - processes incoming fragments */
static void
after_frag( fd_rprtst_ctx_t * ctx,
           ulong               in_idx,
           ulong               seq,
           ulong               sig,
           ulong               sz,
           ulong               tsorig,
           ulong               tspub,
           fd_stem_context_t * stem ) {
  (void)ctx;
  (void)in_idx;
  (void)seq;
  (void)sig;
  (void)sz;
  (void)tsorig;
  (void)tspub;
  (void)stem;

}

/* Implementation of after_credit callback - handles credit management */
static void
after_credit( fd_rprtst_ctx_t * ctx,
             fd_stem_context_t * stem,
             int *               opt_poll_in,
             int *               charge_busy ) {
  (void)stem;
  (void)opt_poll_in;
  (void)charge_busy;

  /* On the first credit cycle, publish gossip peer data */
  if( FD_LIKELY( ctx->first_credit_cycle ) ) {
    ctx->first_credit_cycle = 0;

    FD_LOG_INFO(("Rprtst tile: Running gossip intake and publishing peer data"));

    /* Load repair peers from binary file */
    ulong peer_count = gossip_intake();
    FD_LOG_INFO(("Rprtst tile: peer_count: %lu", peer_count));

    if( FD_LIKELY( ctx->gossip_repair_out_mcache && peer_count > 0 ) ) {
      /* Forge and publish gossip messages to gossip_repai link */
      forge_gossip_messages( ctx );

      ctx->gossip_repair_out_seq = fd_seq_inc( ctx->gossip_repair_out_seq, 1UL );

      FD_LOG_INFO(("Rprtst tile: Published gossip peer data with %lu peers", peer_count));
    } else {
      FD_LOG_WARNING(("Rprtst tile: Failed to publish gossip peer data - mcache=%p, peer_count=%lu",
                     (void*)ctx->gossip_repair_out_mcache, peer_count));
    }
  }
}

static void
unprivileged_init( fd_topo_t *      topo,
                   fd_topo_tile_t * tile ) {

  fd_rprtst_ctx_t * ctx = (fd_rprtst_ctx_t*)fd_ulong_align_up( (ulong)fd_topo_obj_laddr( topo, tile->tile_obj_id ), alignof(fd_rprtst_ctx_t) );

  /* Set flag to run gossip logic on first credit cycle */
  ctx->first_credit_cycle = 1;

  /* Set up output links */
  for( uint out_idx=0U; out_idx<(tile->out_cnt); out_idx++ ) {
    fd_topo_link_t * link = &topo->links[ tile->out_link_id[ out_idx ] ];

    if( 0==strcmp( link->name, "gossip_repai" ) ) {
      ctx->gossip_repair_out_mcache = link->mcache;
      ctx->gossip_repair_out_sync   = fd_mcache_seq_laddr( ctx->gossip_repair_out_mcache );
      ctx->gossip_repair_out_depth  = fd_mcache_depth( ctx->gossip_repair_out_mcache );
      ctx->gossip_repair_out_seq    = fd_mcache_seq_query( ctx->gossip_repair_out_sync );
      ctx->gossip_repair_out_mem    = topo->workspaces[ topo->objs[ link->dcache_obj_id ].wksp_id ].wksp;
      ctx->gossip_repair_out_chunk0 = fd_dcache_compact_chunk0( ctx->gossip_repair_out_mem, link->dcache );
      ctx->gossip_repair_out_wmark  = fd_dcache_compact_wmark ( ctx->gossip_repair_out_mem, link->dcache, link->mtu );
      ctx->gossip_repair_out_chunk  = ctx->gossip_repair_out_chunk0;
    } else {
      FD_LOG_ERR(( "rprtst tile has unexpected output link %s", link->name ));
    }
  }

  ctx->wksp = topo->workspaces[ topo->objs[ tile->tile_obj_id ].wksp_id ].wksp;
}


static ulong
scratch_align( void ) {
  return 128UL;
}

static ulong
scratch_footprint( fd_topo_tile_t const * tile ) {
  (void)tile;
  ulong l = FD_LAYOUT_INIT;
  l = FD_LAYOUT_APPEND( l, alignof(fd_rprtst_ctx_t), sizeof(fd_rprtst_ctx_t) );
  return FD_LAYOUT_FINI( l, scratch_align() );
}

static ulong
populate_allowed_seccomp( fd_topo_t const *      topo,
                         fd_topo_tile_t const * tile,
                         ulong                  out_cnt,
                         struct sock_filter *   out ) {
  (void)topo;
  (void)tile;

  populate_sock_filter_policy_fd_rprtst_tile( out_cnt, out, (uint)fd_log_private_logfile_fd() );
  return sock_filter_policy_fd_rprtst_tile_instr_cnt;
}

static ulong
populate_allowed_fds( fd_topo_t const *      topo,
                     fd_topo_tile_t const * tile,
                     ulong                  out_fds_sz,
                     int *                  out_fds ) {
  (void)topo; (void)tile; (void)out_fds_sz; (void)out_fds;
  return 0UL;
}


#define STEM_BURST (1UL)
#define STEM_LAZY  (128L*3000L)

#define STEM_CALLBACK_CONTEXT_TYPE  fd_rprtst_ctx_t
#define STEM_CALLBACK_CONTEXT_ALIGN alignof(fd_rprtst_ctx_t)

#define STEM_CALLBACK_AFTER_FRAG    after_frag
#define STEM_CALLBACK_AFTER_CREDIT  after_credit

#include "../../disco/stem/fd_stem.c"

fd_topo_run_tile_t fd_tile_rprtst = {
  .name                     = "rprtst",
  .populate_allowed_seccomp = populate_allowed_seccomp,
  .populate_allowed_fds     = populate_allowed_fds,
  .scratch_align            = scratch_align,
  .scratch_footprint        = scratch_footprint,
  .privileged_init          = NULL,
  .unprivileged_init        = unprivileged_init,
  .run                      = stem_run,
};