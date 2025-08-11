/*

Nishk's last minute notes:

Because some of this documentation is being written on the final day of
my internship, I will be discussing some design elements and issues with
the current repair_test command. I am writing this with about an hour
left before I have to return my laptop, so I might be missing somethings
and this will likely be an incomplete dump of my thoughts.

This subcommand is a work in progress.

The current design:

repair_test cmd sets up a topology with the following tiles:

- shredcap
- shred
- sign
- repair
- rprtst

The rprtst tile is used to gossip the repair peers to the repair tile.
Further work for this tile should be done such that shred is replaced by
the repair test (rprtst) tile. This can be done by taking the minimal
neccessary net->shred control and using it as the rprtst tile.

Gossip is specifically recieved by a python script which has no external
dependencies. This python script dumps the cleaned testnet contact list
into a binary which perfectly matches the message format of the
gossip_repair link. The future goals for this would be to set it up such
that the user can include the outputted json from the rpc endpoint into
the repair_test command. This would allow for a more dynamic use. The
file path could be passed into the tile context and decoded/parsed into
the bytes needed in the link.

Shredcap is used to track the timing of the repair requests and
responses. These functions are extremely buggy. I would have liked the
opportunity to spend more time on them, but I did not get the chance.
This is a pretty broken design, specifically I am hitting a
nonreproducible edge case where the end case is not being hit, even when
we are fully done repairing. This happens 1 in every ~8 runs.

There is an issue with the link setups. I also did not get the chance to
look too deeply into this, but I believe the socket tile links are not
being set up correctly. If I remove the net_gossip link, the repair
test will not recieve messages that are sent to the listen repair
port.

The current code path is not ideal, and I would have liked to spend
more time on it, but I did not get the chance.

Here is my toml for when I run this command in case of a reproducibility
error, most paramters aren't used, just copied over from another toml:

[layout]
    exec_tile_count = 10
    sign_tile_count = 2
[gossip]
    port = $(shuf -i 8000-10000 -n 1)
    entrypoints = [
      \"141.98.216.90:8001\",
      \"35.209.131.19:8001\",
      \"entrypoint.testnet.solana.com:8001\",
      \"entrypoint2.testnet.solana.com:8001\",
      \"entrypoint3.testnet.solana.com:8001\"
    ]
[blockstore]
    shred_max = 16777216
    block_max = 131072
    txn_max = 1048576
    idx_max = 8192
    alloc_max = 10737418240
    file = \"/dev/shm/testnet.blockstore\"
[tiles]
    [tiles.shred]
        max_pending_shred_sets = 16384
        shred_listen_port = 1000
    [tiles.repair]
        repair_serve_listen_port = $(shuf -i 8000-10000 -n 1)
        slot_max = 16384
    [tiles.metric]
        prometheus_listen_address = \"0.0.0.0\"
        prometheus_listen_port = 7999
[snapshots]
    cluster_name = \"testnet\"
[funk]
    max_account_records = 150000000
    heap_size_gib = 140
    max_database_transactions = 2000
[runtime]
    heap_size_gib = 50
    [runtime.limits]
        max_banks = 128
[recorder]
    timeout_ns = 200000000
[consensus]
    vote = false
    expected_shred_version = 9065
[paths]
    identity_key = \"/home/npatelintern/keys/fd-identity-keypair.json\"
    vote_account = \"/home/npatelintern/keys/fd-vote-keypair.json\"
    snapshots = \"/data/npatelintern/testnet/snapshots\"
[tiles.shredcap]
    enabled = true
    download = false
    folder_path = \"/data/npatelintern/ledger-dir/shredcap-$DATE:$TIME\"
[log]
    path = \"/data/npatelintern/ledger-dir/testnet-repair-$DATE:$TIME.log\"
[net]
    provider = \"socket\"
[development]
    sandbox = false
*/


/* The repair_test command spawns a smaller topology for testing soley
   the repair tile.  This is a standalone application, and it can be run
   in mainnet, testnet and/or a private cluster.

   This tool is used for testing the repair tile in a standalone manner.
   It is not meant to be used in a production environment.

   This commmand uses three unique inputs:

   --manifest-path: The path to the manifest file.
   --start-slot: The user-defined start slot of the repair test.
   --end-slot: The user-defined end slot of the repair test.

   NOTICE: This subcommand requires the shred_listen_port to be set in
   the configuration file to a value [0, 1023). This is because the
   privilaged port will not be read from, and therfore, turbine shreds
   will not be recieved by the client. Furthermore, the shredcap tile
   must be running as it is used for metrics tracking.

   ex.  [tiles.shred]
          shred_listen_port = 1000
        [net]
          provider = "socket"
        [tiles.shredcap]


   The start-slot will be utilized as the beginning slot for the repair
   process (i.e. frontier slot). The end-slot signfies the slot for
   which the tool will repair up to (i.e. treated as first turbine).
   Currently, the manifest is the easiest way to transmit information
   into the repair tile, but TODO: find a simpler way to do this.

   The command will then print the statistics of the repair test:
   - End slot: The end slot of the repair test.
   - Start slot: The start slot of the repair test.
   - Slots repaired: The number of slots repaired.
   - Total repair requests: The number of repair requests sent.
   - Total received responses: The number of repair responses received.
   - Total repair time: The total time taken for the repair test.
   - Milliseconds per slot: The average time taken per slot.
   */

   #include "../../../disco/net/fd_net_tile.h"
   #include "../../../disco/tiles.h"
   #include "../../../disco/topo/fd_topob.h"
   #include "../../../disco/topo/fd_cpu_topo.h"
   #include "../../../util/pod/fd_pod_format.h"
   #include "../../../util/tile/fd_tile_private.h"

   #include "../../firedancer/topology.h"
   #include "../../shared/commands/configure/configure.h"
   #include "../../shared/commands/run/run.h"
   #include "../../shared/fd_config.h"
   #include "../../shared_dev/commands/dev.h"
   #include "../../../disco/tiles.h"
   #include "../../../disco/topo/fd_topob.h"
   #include "../../../util/pod/fd_pod_format.h"
   #include "../../../flamenco/runtime/fd_runtime.h"
   #include "../../../ballet/shred/fd_shred.h"
   #include "../../../discof/reasm/fd_reasm.h"
   #include "../../../discof/restore/utils/fd_ssmsg.h"
   #include "../../../tango/mcache/fd_mcache.h"
   #include "../../../tango/dcache/fd_dcache.h"

   #include <unistd.h>
   #include <sys/socket.h>
   #include <netdb.h>
   #include <netinet/in.h>

   extern fd_topo_obj_callbacks_t * CALLBACKS[];
   fd_topo_run_tile_t fdctl_tile_run( fd_topo_tile_t const * tile );

   static void
   forge_turbine_slot( ulong end_slot, void * wksp_laddr, ulong chunk, fd_frag_meta_t * mcache, ulong depth ) {
     /* Create some test data — a valid repair request, replaced slot
        number with the end_slot value for the repair tile. */
     uchar test_data[] = {
       0x1b, 0x2d, 0xa2, 0x80, 0x04, 0xdf, 0xee, 0x32, 0x44, 0xd1,
       0xe4, 0x66, 0xa5, 0x21, 0x99, 0x70, 0xf8, 0x45, 0x6f, 0x68,
       0x6d, 0x96, 0xb8, 0x3a, 0xbb, 0x05, 0xab, 0xd2, 0xea, 0x37,
       0x72, 0xef, 0xad, 0x5b, 0x96, 0x3e, 0x40, 0xce, 0x51, 0x86,
       0x43, 0xf6, 0x7c, 0xf6, 0xfe, 0xe7, 0x09, 0xec, 0x31, 0x46,
       0x2d, 0x22, 0xb7, 0xbc, 0xf2, 0x97, 0xdc, 0xb7, 0xad, 0xe5,
       0x16, 0x81, 0x4c, 0x07, 0x96,

       /* These 4 bytes are used for shred struct slot field */
       0xFF, 0xFF, 0xFF, 0xFF,

       0x00, 0x00, 0x00, 0x00, 0x89, 0x00, 0x00, 0x00, 0x69, 0x23,
       0x80, 0x00, 0x00, 0x00, 0x01, 0x00, 0x0d, 0x1b, 0x04, 0x00
     };
     ulong data_sz = sizeof(test_data);
     fd_shred_t *shred = (fd_shred_t *)test_data;
     shred->slot = end_slot;

     void * chunk_laddr = fd_chunk_to_laddr( wksp_laddr, chunk );
     fd_memcpy( chunk_laddr, test_data, data_sz );

     ulong seq = 0UL;
     ulong sig = 0UL;
     ulong ctl = fd_frag_meta_ctl( 0UL, 1, 1, 0 );
     ulong ts = (ulong)fd_frag_meta_ts_comp( fd_tickcount() );

     FD_LOG_HEXDUMP_NOTICE(("Forged turbine slot", test_data, data_sz));
     sleep(10);
     fd_mcache_publish( mcache, depth, seq, sig, chunk, data_sz, ctl, ts, ts );
   }

   static void FD_FN_UNUSED
   forge_gossip_messages( fd_frag_meta_t * mcache, ulong depth, void * wksp_laddr, ulong chunk_start ) {
     // Construct path to repair peers binary file
     char repair_peers_path[PATH_MAX];
     char *file_dir = strdup(__FILE__);
     char *last_slash = strrchr(file_dir, '/');
     if (last_slash) {
       *last_slash = '\0';
       snprintf(repair_peers_path, sizeof(repair_peers_path), "%s/repair_peers.bin", file_dir);
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
         // Allocate buffer for all peer data
         void * chunk_laddr = fd_chunk_to_laddr( wksp_laddr, chunk_start );

         // Read all peer data into the chunk
         long bytes_read = (long)fread( chunk_laddr, 1, (ulong)file_size, file );

         if (bytes_read == file_size) {
           ulong seq = 0UL;
           ulong sig = 0UL;
           ulong ctl = fd_frag_meta_ctl( 0UL, 1, 1, 0 );
           ulong ts = (ulong)fd_frag_meta_ts_comp( fd_tickcount() );


          //  uchar data[38] = {

          //  }

           // Publish single message with peer_count as the data size
           fd_mcache_publish( mcache, depth, seq, sig, chunk_start, peer_count, ctl, ts, ts );

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

   static ulong FD_FN_UNUSED
   gossip_intake( void ) {
     // Construct path to Python helper script and run it to generate/update repair peers data
     char script_path[PATH_MAX];
     char repair_peers_path[PATH_MAX];
     char *file_dir = strdup(__FILE__);
     char *last_slash = strrchr(file_dir, '/');
     if (last_slash) {
       *last_slash = '\0';
       snprintf(script_path, sizeof(script_path), "%s/repair_test_helper.py", file_dir);
       snprintf(repair_peers_path, sizeof(repair_peers_path), "%s/repair_peers.bin", file_dir);
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

   static ulong
   link_permit_no_producers( fd_topo_t * topo, char * link_name ) {
     ulong found = 0UL;
     for( ulong link_i = 0UL; link_i < topo->link_cnt; link_i++ ) {
       if( !strcmp( topo->links[ link_i ].name, link_name ) ) {
         topo->links[ link_i ].permit_no_producers = 1;
         found++;
       }
     }
     return found;
   }

   static ulong
   link_permit_no_consumers( fd_topo_t * topo, char * link_name ) {
     ulong found = 0UL;
     for( ulong link_i = 0UL; link_i < topo->link_cnt; link_i++ ) {
       if( !strcmp( topo->links[ link_i ].name, link_name ) ) {
         topo->links[ link_i ].permit_no_consumers = 1;
         found++;
       }
     }
     return found;
   }

   /* repair_test_topo is a subset of "src/app/firedancer/topology.c" at commit
      0d8386f4f305bb15329813cfe4a40c3594249e96, slightly modified to work
      as a repair profiler.  TODO ideally, one should invoke the firedancer
      topology first, and exclude the parts that are not needed, instead of
      manually generating new topologies for every command.  This would
      also guarantee that the profiler is replicating (as close as possible)
      the full topology. */
   static void
   repair_test_topo( config_t * config ) {
     resolve_gossip_entrypoints( config );

     ulong net_tile_cnt    = config->layout.net_tile_count;
     ulong shred_tile_cnt  = config->layout.shred_tile_count;
     ulong quic_tile_cnt   = config->layout.quic_tile_count;
     ulong sign_tile_cnt   = config->firedancer.layout.sign_tile_count;

     fd_topo_t * topo = { fd_topob_new( &config->topo, config->name ) };
     topo->max_page_size = fd_cstr_to_shmem_page_sz( config->hugetlbfs.max_page_size );
     topo->gigantic_page_threshold = config->hugetlbfs.gigantic_page_threshold_mib << 20;

     /*             topo, name */
     fd_topob_wksp( topo, "metric_in"    );
     fd_topob_wksp( topo, "net_shred"    );
     fd_topob_wksp( topo, "net_repair"   );
     fd_topob_wksp( topo, "net_quic"     );

     fd_topob_wksp( topo, "shred_repair" );
     fd_topob_wksp( topo, "stake_out"    );

     fd_topob_wksp( topo, "poh_shred"    );

     fd_topob_wksp( topo, "shred_sign"   );
     fd_topob_wksp( topo, "sign_shred"   );

     fd_topob_wksp( topo, "repair_sign"  );
     fd_topob_wksp( topo, "sign_repair"  );

     fd_topob_wksp( topo, "repair_repla" );

     fd_topob_wksp( topo, "shred"        );
     fd_topob_wksp( topo, "sign"         );
     fd_topob_wksp( topo, "repair"       );
     fd_topob_wksp( topo, "metric"       );
     fd_topob_wksp( topo, "fec_sets"     );
     fd_topob_wksp( topo, "snap_out"     );

     fd_topob_wksp( topo, "rprtst"       );

     fd_topob_wksp( topo, "slot_fseqs"   ); /* fseqs for marked slots eg. turbine slot */

     #define FOR(cnt) for( ulong i=0UL; i<cnt; i++ )

     ulong pending_fec_shreds_depth = fd_ulong_min( fd_ulong_pow2_up( config->tiles.shred.max_pending_shred_sets * FD_REEDSOL_DATA_SHREDS_MAX ), USHORT_MAX + 1 /* dcache max */ );

    /*                                  topo, link_name,      wksp_name,      depth,                                    mtu,                           burst */
    FOR(quic_tile_cnt)   fd_topob_link( topo, "quic_net",     "net_quic",     config->net.ingress_buffer_size,          FD_NET_MTU,                    1UL );
    FOR(shred_tile_cnt)  fd_topob_link( topo, "shred_net",    "net_shred",    config->net.ingress_buffer_size,          FD_NET_MTU,                    1UL );

    /**/                 fd_topob_link( topo, "stake_out",    "stake_out",    128UL,                                    40UL + 40200UL * 40UL,         1UL );

    FOR(shred_tile_cnt)  fd_topob_link( topo, "shred_sign",   "shred_sign",   128UL,                                    32UL,                          1UL );
    FOR(shred_tile_cnt)  fd_topob_link( topo, "sign_shred",   "sign_shred",   128UL,                                    64UL,                          1UL );

    /**/                 fd_topob_link( topo, "gossip_repai", "rprtst", 128UL,                                    40200UL * 38UL, 1UL );

    /**/                 fd_topob_link( topo, "repair_net",   "net_repair",   config->net.ingress_buffer_size,          FD_NET_MTU,                    1UL );

    /* This extra shred_repair link serves as a dedicated, single-use
       buffer for injecting the initial shred into the repair tile. It
       effectively acts as the "first turbine" for the repair process,
       enabling the forest to initialize its turbine slot and providing
       an orphaned slot for subsequent repairs. The only producer for
       this link is this subcommand itself. */
    FOR(shred_tile_cnt+1)fd_topob_link( topo, "shred_repair", "shred_repair", pending_fec_shreds_depth,                 FD_SHRED_REPAIR_MTU,           2UL /* at most 2 msgs per after_frag */ );

    FOR(shred_tile_cnt)  fd_topob_link( topo, "repair_shred", "shred_repair", pending_fec_shreds_depth,                 sizeof(fd_ed25519_sig_t),      1UL );

    /**/                 fd_topob_link( topo, "ping_sign",    "repair_sign",  128UL,                                    2048UL,                        1UL );
    /**/                 fd_topob_link( topo, "sign_ping",    "sign_repair",  128UL,                                    sizeof(fd_ed25519_sig_t),      1UL );
    FOR(sign_tile_cnt-1) fd_topob_link( topo, "repair_sign",  "repair_sign",  128UL,                                    2048UL,                        1UL );
    FOR(sign_tile_cnt-1) fd_topob_link( topo, "sign_repair",  "sign_repair",  1024UL,                                   sizeof(fd_ed25519_sig_t),      1UL );

    /**/                 fd_topob_link( topo, "repair_repla", "repair_repla", 65536UL,                                  sizeof(fd_reasm_fec_t),    1UL );
    /**/                 fd_topob_link( topo, "poh_shred",    "poh_shred",    16384UL,                                  USHORT_MAX,                    1UL );


    FD_TEST( sizeof(fd_snapshot_manifest_t)<=(5UL*(1UL<<30UL)) );
    /**/                 fd_topob_link( topo, "snap_out",     "snap_out",     2UL,                                      5UL*(1UL<<30UL),               1UL );

     ushort parsed_tile_to_cpu[ FD_TILE_MAX ];
     /* Unassigned tiles will be floating, unless auto topology is enabled. */
     for( ulong i=0UL; i<FD_TILE_MAX; i++ ) parsed_tile_to_cpu[ i ] = USHORT_MAX;

     int is_auto_affinity = !strcmp( config->layout.affinity, "auto" );
     int is_bench_auto_affinity = !strcmp( config->development.bench.affinity, "auto" );

     if( FD_UNLIKELY( is_auto_affinity != is_bench_auto_affinity ) ) {
       FD_LOG_ERR(( "The CPU affinity string in the configuration file under [layout.affinity] and [development.bench.affinity] must all be set to 'auto' or all be set to a specific CPU affinity string." ));
     }

     fd_topo_cpus_t cpus[1];
     fd_topo_cpus_init( cpus );

     ulong affinity_tile_cnt = 0UL;
     if( FD_LIKELY( !is_auto_affinity ) ) affinity_tile_cnt = fd_tile_private_cpus_parse( config->layout.affinity, parsed_tile_to_cpu );

     ulong tile_to_cpu[ FD_TILE_MAX ] = {0};
     for( ulong i=0UL; i<affinity_tile_cnt; i++ ) {
       if( FD_UNLIKELY( parsed_tile_to_cpu[ i ]!=USHORT_MAX && parsed_tile_to_cpu[ i ]>=cpus->cpu_cnt ) )
         FD_LOG_ERR(( "The CPU affinity string in the configuration file under [layout.affinity] specifies a CPU index of %hu, but the system "
                     "only has %lu CPUs. You should either change the CPU allocations in the affinity string, or increase the number of CPUs "
                     "in the system.",
                     parsed_tile_to_cpu[ i ], cpus->cpu_cnt ));
       tile_to_cpu[ i ] = fd_ulong_if( parsed_tile_to_cpu[ i ]==USHORT_MAX, ULONG_MAX, (ulong)parsed_tile_to_cpu[ i ] );
     }

     fd_topos_net_tiles( topo, config->layout.net_tile_count, &config->net, config->tiles.netlink.max_routes, config->tiles.netlink.max_peer_routes, config->tiles.netlink.max_neighbors, tile_to_cpu );

     FOR(net_tile_cnt) fd_topos_net_rx_link( topo, "net_gossip", i, config->net.ingress_buffer_size );
     FOR(net_tile_cnt) fd_topos_net_rx_link( topo, "net_repair", i, config->net.ingress_buffer_size );
     FOR(net_tile_cnt) fd_topos_net_rx_link( topo, "net_quic",   i, config->net.ingress_buffer_size );
     FOR(net_tile_cnt) fd_topos_net_rx_link( topo, "net_shred",  i, config->net.ingress_buffer_size );

         /*                                              topo, tile_name, tile_wksp, metrics_wksp, cpu_idx,                       is_agave, uses_keyswitch */
    FOR(shred_tile_cnt)              fd_topob_tile( topo, "shred",   "shred",   "metric_in",  tile_to_cpu[ topo->tile_cnt ], 0,        1 );
    FOR(sign_tile_cnt)               fd_topob_tile( topo, "sign",    "sign",    "metric_in",  tile_to_cpu[ topo->tile_cnt ], 0,        1 );
    /**/                             fd_topob_tile( topo, "metric",  "metric",  "metric_in",  tile_to_cpu[ topo->tile_cnt ], 0,        0 );
    fd_topo_tile_t * repair_tile =   fd_topob_tile( topo, "repair",  "repair",  "metric_in",  tile_to_cpu[ topo->tile_cnt ], 0,        0 );
    /**/                             fd_topob_tile( topo, "rprtst", "rprtst", "metric_in",  tile_to_cpu[ topo->tile_cnt ], 0,        0 );

     /* Setup a shared wksp object for fec sets. */

     ulong shred_depth = 65536UL; /* from fdctl/topology.c shred_store link. MAKE SURE TO KEEP IN SYNC. */
     ulong fec_set_cnt = shred_depth + config->tiles.shred.max_pending_shred_sets + 4UL;
     ulong fec_sets_sz = fec_set_cnt*sizeof(fd_shred34_t)*4; /* mirrors # of dcache entires in frankendancer */
     fd_topo_obj_t * fec_sets_obj = setup_topo_fec_sets( topo, "fec_sets", shred_tile_cnt*fec_sets_sz );
     for( ulong i=0UL; i<shred_tile_cnt; i++ ) {
       fd_topo_tile_t * shred_tile = &topo->tiles[ fd_topo_find_tile( topo, "shred", i ) ];
       fd_topob_tile_uses( topo, shred_tile,  fec_sets_obj, FD_SHMEM_JOIN_MODE_READ_WRITE );
     }
     fd_topob_tile_uses( topo, repair_tile, fec_sets_obj, FD_SHMEM_JOIN_MODE_READ_ONLY );
     FD_TEST( fd_pod_insertf_ulong( topo->props, fec_sets_obj->id, "fec_sets" ) );

     /* There's another special fseq that's used to communicate the shred
       version from the Agave boot path to the shred tile. */
     fd_topo_obj_t * poh_shred_obj = fd_topob_obj( topo, "fseq", "poh_shred" );

     /* root_slot is an fseq marking the validator's current Tower root. */

     fd_topo_obj_t * root_slot_obj = fd_topob_obj( topo, "fseq", "slot_fseqs" );
     FD_TEST( fd_pod_insertf_ulong( topo->props, root_slot_obj->id, "root_slot" ) );

     /* turbine_slot0 is an fseq marking the slot number of the first shred
        we observed from Turbine.  This is a useful heuristic for
        determining when replay has progressed past the slot in which we
        last voted.  The idea is once replay has proceeded past the slot
        from which validator stopped replaying and therefore also stopped
        voting (crashed, shutdown, etc.), it will have "read-back" its
        latest tower in the ledger.  Note this logic is not true in the
        case our latest tower vote was for a minority fork. */

     fd_topo_obj_t * turbine_slot0_obj = fd_topob_obj( topo, "fseq", "slot_fseqs" );
     fd_topob_tile_uses( topo, repair_tile, turbine_slot0_obj, FD_SHMEM_JOIN_MODE_READ_WRITE );
     FD_TEST( fd_pod_insertf_ulong( topo->props, turbine_slot0_obj->id, "turbine_slot0" ) );

     /* turbine_slot is an fseq marking the highest slot we've observed on
        a shred.  This is continuously updated as the validator is running
        and is used to determine whether the validator is caught up with
        the rest of the cluster. */

     fd_topo_obj_t * turbine_slot_obj = fd_topob_obj( topo, "fseq", "slot_fseqs" );
     fd_topob_tile_uses( topo, repair_tile, turbine_slot_obj, FD_SHMEM_JOIN_MODE_READ_WRITE );
     FD_TEST( fd_pod_insertf_ulong( topo->props, turbine_slot_obj->id, "turbine_slot" ) );

     for( ulong i=0UL; i<shred_tile_cnt; i++ ) {
       fd_topo_tile_t * shred_tile = &topo->tiles[ fd_topo_find_tile( topo, "shred", i ) ];
       fd_topob_tile_uses( topo, shred_tile, poh_shred_obj, FD_SHMEM_JOIN_MODE_READ_ONLY );
     }
     FD_TEST( fd_pod_insertf_ulong( topo->props, poh_shred_obj->id, "poh_shred" ) );

     if( FD_LIKELY( !is_auto_affinity ) ) {
       if( FD_UNLIKELY( affinity_tile_cnt<topo->tile_cnt ) )
         FD_LOG_ERR(( "The topology you are using has %lu tiles, but the CPU affinity specified in the config tile as [layout.affinity] only provides for %lu cores. "
                     "You should either increase the number of cores dedicated to Firedancer in the affinity string, or decrease the number of cores needed by reducing "
                     "the total tile count. You can reduce the tile count by decreasing individual tile counts in the [layout] section of the configuration file.",
                     topo->tile_cnt, affinity_tile_cnt ));
       if( FD_UNLIKELY( affinity_tile_cnt>topo->tile_cnt ) )
         FD_LOG_WARNING(( "The topology you are using has %lu tiles, but the CPU affinity specified in the config tile as [layout.affinity] provides for %lu cores. "
                         "Not all cores in the affinity will be used by Firedancer. You may wish to increase the number of tiles in the system by increasing "
                         "individual tile counts in the [layout] section of the configuration file.",
                         topo->tile_cnt, affinity_tile_cnt ));
     }

    /*                                      topo, tile_name, tile_kind_id, fseq_wksp,   link_name,      link_kind_id, reliable,            polled */
    for( ulong j=0UL; j<shred_tile_cnt; j++ )
                    fd_topos_tile_in_net(  topo,                          "metric_in", "shred_net",    j,            FD_TOPOB_UNRELIABLE, FD_TOPOB_POLLED ); /* No reliable consumers of networking fragments, may be dropped or overrun */
    for( ulong j=0UL; j<quic_tile_cnt; j++ )
                    fd_topos_tile_in_net(  topo,                          "metric_in", "quic_net",     j,            FD_TOPOB_UNRELIABLE, FD_TOPOB_POLLED ); /* No reliable consumers of networking fragments, may be dropped or overrun */

    /**/             fd_topos_tile_in_net(  topo,                          "metric_in", "repair_net",   0UL,          FD_TOPOB_UNRELIABLE, FD_TOPOB_POLLED ); /* No reliable consumers of networking fragments, may be dropped or overrun */

    FOR(shred_tile_cnt) for( ulong j=0UL; j<net_tile_cnt; j++ )
                        fd_topob_tile_in(  topo, "shred",  i,             "metric_in", "net_shred",     j,            FD_TOPOB_UNRELIABLE,   FD_TOPOB_POLLED ); /* No reliable consumers of networking fragments, may be dropped or overrun */
    FOR(shred_tile_cnt)  fd_topob_tile_in(  topo, "shred",  i,             "metric_in", "poh_shred",     0UL,          FD_TOPOB_RELIABLE,     FD_TOPOB_POLLED );
    FOR(shred_tile_cnt)  fd_topob_tile_in(  topo, "shred",  i,             "metric_in", "stake_out",     0UL,          FD_TOPOB_RELIABLE,     FD_TOPOB_POLLED );
    FOR(shred_tile_cnt)  fd_topob_tile_out( topo, "shred",  i,                          "shred_repair",  i                                                    );
    FOR(shred_tile_cnt)  fd_topob_tile_out( topo, "shred",  i,                          "shred_net",     i                                                    );

    FOR(shred_tile_cnt)  fd_topob_tile_in(  topo, "shred",  i,             "metric_in",  "repair_shred", i,            FD_TOPOB_RELIABLE,   FD_TOPOB_POLLED   );

    /**/                 fd_topob_tile_out( topo, "repair",  0UL,                       "repair_net",    0UL                                                  );
    /**/                 fd_topob_tile_out( topo, "rprtst", 0UL,                       "gossip_repai",  0UL                                                  );

    for( ulong i=0UL; i<shred_tile_cnt; i++ ) {
      /**/               fd_topob_tile_in(  topo, "sign",   0UL,           "metric_in", "shred_sign",    i,            FD_TOPOB_UNRELIABLE, FD_TOPOB_POLLED   );
      /**/               fd_topob_tile_out( topo, "shred",  i,                          "shred_sign",    i                                                    );
      /**/               fd_topob_tile_in(  topo, "shred",  i,             "metric_in", "sign_shred",    i,            FD_TOPOB_UNRELIABLE, FD_TOPOB_UNPOLLED );
      /**/               fd_topob_tile_out( topo, "sign",   0UL,                        "sign_shred",    i                                                    );
    }


    FOR(net_tile_cnt)    fd_topob_tile_in(  topo, "repair",  0UL,          "metric_in", "net_repair",    i,            FD_TOPOB_UNRELIABLE, FD_TOPOB_POLLED   ); /* No reliable consumers of networking fragments, may be dropped or overrun */
    /**/                 fd_topob_tile_in(  topo, "repair",  0UL,          "metric_in", "gossip_repai",  0UL,          FD_TOPOB_RELIABLE,   FD_TOPOB_POLLED   );
    /**/                 fd_topob_tile_in(  topo, "repair",  0UL,          "metric_in", "stake_out",     0UL,          FD_TOPOB_UNRELIABLE, FD_TOPOB_POLLED   );
                        fd_topob_tile_in(  topo, "repair",  0UL,          "metric_in", "snap_out",      0UL,          FD_TOPOB_RELIABLE,   FD_TOPOB_POLLED   );
    FOR(shred_tile_cnt+1)fd_topob_tile_in(  topo, "repair",  0UL,          "metric_in", "shred_repair",  i,            FD_TOPOB_RELIABLE,   FD_TOPOB_POLLED   );

    /**/                 fd_topob_tile_in(  topo, "sign",   0UL,         "metric_in",  "ping_sign",    0UL,    FD_TOPOB_RELIABLE,   FD_TOPOB_POLLED   );
    /**/                 fd_topob_tile_out( topo, "repair", 0UL,                       "ping_sign",    0UL                                            );
    /**/                 fd_topob_tile_out( topo, "repair", 0UL,                       "repair_repla", 0UL                                            );
    FOR(shred_tile_cnt)  fd_topob_tile_out( topo, "repair", 0UL,                       "repair_shred", i                                              );
    /**/                 fd_topob_tile_out( topo, "sign",   0UL,                       "sign_ping",    0UL                                            );

    FOR(sign_tile_cnt-1) fd_topob_tile_out( topo, "repair", 0UL,                        "repair_sign",  i                                              );
    FOR(sign_tile_cnt-1) fd_topob_tile_in ( topo, "sign",   i+1,           "metric_in", "repair_sign",  i,      FD_TOPOB_RELIABLE,   FD_TOPOB_POLLED   );
    FOR(sign_tile_cnt-1) fd_topob_tile_out( topo, "sign",   i+1,                        "sign_repair",  i                                              );
    FOR(sign_tile_cnt-1) fd_topob_tile_in ( topo, "repair", 0UL,           "metric_in", "sign_repair",  i,      FD_TOPOB_UNRELIABLE, FD_TOPOB_POLLED   );
      /**/               fd_topob_tile_in ( topo, "repair", 0UL,           "metric_in", "sign_ping",    0UL,    FD_TOPOB_UNRELIABLE, FD_TOPOB_UNPOLLED );

     if( 1 ) {
       fd_topob_wksp( topo, "scap" );

       fd_topob_wksp( topo, "repair_scap" );
       fd_topob_wksp( topo, "replay_scap" );

       fd_topo_tile_t * scap_tile = fd_topob_tile( topo, "scap", "scap", "metric_in", tile_to_cpu[ topo->tile_cnt ], 0, 0 );

       fd_topob_link( topo, "repair_scap", "repair_scap", 128UL, FD_SLICE_MAX_WITH_HEADERS, 1UL );
       fd_topob_link( topo, "replay_scap", "replay_scap", 128UL, sizeof(fd_hash_t)+sizeof(ulong), 1UL );

       fd_topob_tile_in(  topo, "scap", 0UL, "metric_in", "repair_net", 0UL, FD_TOPOB_UNRELIABLE, FD_TOPOB_POLLED );
       for( ulong j=0UL; j<net_tile_cnt; j++ ) {
         fd_topob_tile_in(  topo, "scap", 0UL, "metric_in", "net_shred", j, FD_TOPOB_UNRELIABLE, FD_TOPOB_POLLED );
       }
       for( ulong j=0UL; j<shred_tile_cnt; j++ ) {
         fd_topob_tile_in(  topo, "scap", 0UL, "metric_in", "shred_repair", j, FD_TOPOB_UNRELIABLE, FD_TOPOB_POLLED );
       }
       fd_topob_tile_in( topo, "scap", 0UL, "metric_in", "gossip_repai", 0UL, FD_TOPOB_UNRELIABLE, FD_TOPOB_POLLED );

       fd_topob_tile_in( topo, "scap", 0UL, "metric_in", "repair_scap", 0UL, FD_TOPOB_RELIABLE, FD_TOPOB_POLLED );
       fd_topob_tile_in( topo, "scap", 0UL, "metric_in", "replay_scap", 0UL, FD_TOPOB_RELIABLE, FD_TOPOB_POLLED );

       fd_topob_tile_uses( topo, scap_tile, root_slot_obj, FD_SHMEM_JOIN_MODE_READ_WRITE );
       fd_topob_tile_out( topo, "scap", 0UL, "stake_out", 0UL );
       fd_topob_tile_out( topo, "scap", 0UL, "snap_out",  0UL );
     }

     FD_TEST( link_permit_no_producers( topo, "quic_net"     ) == quic_tile_cnt );
     FD_TEST( link_permit_no_producers( topo, "poh_shred"    ) == 1UL           );
     FD_TEST( link_permit_no_producers( topo, "repair_scap"  ) == 1UL           );
     FD_TEST( link_permit_no_producers( topo, "replay_scap"  ) == 1UL           );

     /* Set permit_no_producers on the last shred_repair link (index
        shred_tile_cnt). Explained in link initialization above. */
     for( ulong link_i = 0UL; link_i < topo->link_cnt; link_i++ ) {
       if( !strcmp( topo->links[ link_i ].name, "shred_repair" ) &&
           topo->links[ link_i ].kind_id == shred_tile_cnt ) {
         topo->links[ link_i ].permit_no_producers = 1;
         FD_LOG_NOTICE(( "Set permit_no_producers on shred_repair link %lu (kind_id %lu)", link_i, shred_tile_cnt ));
         break;
       }
     }

     FD_TEST( link_permit_no_consumers( topo, "net_quic"     ) == quic_tile_cnt );
     FD_TEST( link_permit_no_consumers( topo, "repair_repla" ) == 1UL           );
     FD_TEST( link_permit_no_consumers( topo, "net_gossip"   ) == net_tile_cnt );

     FOR(net_tile_cnt) fd_topos_net_tile_finish( topo, i );

     for( ulong i=0UL; i<topo->tile_cnt; i++ ) {
       fd_topo_tile_t * tile = &topo->tiles[ i ];
       if( !fd_topo_configure_tile( tile, config ) ) {
         FD_LOG_ERR(( "unknown tile name %lu `%s`", i, tile->name ));
       }
     }

     if( FD_UNLIKELY( is_auto_affinity ) ) fd_topob_auto_layout( topo, 0 );

     fd_topob_finish( topo, CALLBACKS );

     config->topo = *topo;
   }

  extern int * fd_log_private_shared_lock;

  void
  repair_test_cmd_args( int *    pargc,
                  char *** pargv,
                  args_t * args ) {

    if( FD_UNLIKELY( !*pargc ) ) FD_LOG_ERR(( "usage: repair_test --manifest-path <manifest_path> --start-slot <start_slot> --end-slot <end_slot>" ));

    char const * manifest_path = fd_env_strip_cmdline_cstr( pargc, pargv, "--manifest-path", NULL, "unknown" );
    fd_cstr_fini( fd_cstr_append_cstr_safe( fd_cstr_init( args->repair_test.manifest_path ), manifest_path, sizeof(args->repair_test.manifest_path)-1UL ) );

    args->repair_test.start_slot = fd_env_strip_cmdline_ulong( pargc, pargv, "--start-slot", NULL, 0UL );
    args->repair_test.end_slot   = fd_env_strip_cmdline_ulong( pargc, pargv, "--end-slot",   NULL, 0UL );

    FD_LOG_NOTICE(( "repair_test manifest_path %s, start_slot %lu, end_slot %lu",
                    args->repair_test.manifest_path, args->repair_test.start_slot, args->repair_test.end_slot ));
  }

  static void
  repair_test_cmd_fn( args_t *   args,
                config_t * config ) {

    FD_LOG_NOTICE(( "Repair_test profiler topo (start_slot: %lu, end_slot: %lu)",
                    args->repair_test.start_slot, args->repair_test.end_slot ));

    memset( &config->topo, 0, sizeof(config->topo) );
    repair_test_topo( config );

    for( ulong i=0UL; i<config->topo.tile_cnt; i++ ) {
      fd_topo_tile_t * tile = &config->topo.tiles[ i ];
      if( FD_UNLIKELY( !strcmp( tile->name, "scap" ) ) ) {
        /* This is not part of the config, and it must be set manually
          on purpose as a safety mechanism. */
        tile->shredcap.enable_publish_stake_weights = 1;
        strncpy( tile->shredcap.manifest_path, args->repair_test.manifest_path, PATH_MAX );
        tile->shredcap.start_slot = args->repair_test.start_slot;
        tile->shredcap.end_slot = args->repair_test.end_slot;
        tile->shredcap.repair_test = 1;
      }
    }

    fd_topo_print_log( 1, &config->topo );

    args_t configure_args = {
      .configure.command = CONFIGURE_CMD_INIT,
    };
    for( ulong i=0UL; STAGES[ i ]; i++ ) {
      configure_args.configure.stages[ i ] = STAGES[ i ];
    }
    configure_cmd_fn( &configure_args, config );
    if( 0==strcmp( config->net.provider, "xdp" ) ) {
      fd_xdp_fds_t fds = fd_topo_install_xdp( &config->topo, config->net.bind_address_parsed );
      (void)fds;
    }

    run_firedancer_init( config, 1 );

    fd_log_private_shared_lock[ 1 ] = 0;
    fd_topo_join_workspaces( &config->topo, FD_SHMEM_JOIN_MODE_READ_WRITE );

    fd_topo_run_single_process( &config->topo, 0, config->uid, config->gid, fdctl_tile_run );

    /* Find the shred_repair link for publishing */
    ulong last_shred_repair_idx = 0;
    fd_topo_link_t * shred_repair_link = NULL;

    for( ulong link_i = 0UL; link_i < config->topo.link_cnt; link_i++ ) {
      if( !strcmp( config->topo.links[ link_i ].name, "shred_repair" ) &&
          config->topo.links[ link_i ].kind_id >= last_shred_repair_idx ) {
        last_shred_repair_idx = config->topo.links[ link_i ].kind_id;
        shred_repair_link = &config->topo.links[ link_i ];
      }
    }

    if (FD_UNLIKELY( !shred_repair_link )) {
      FD_LOG_ERR(( "Shred repair link not found"));
    }

    // Setup shred repair link
    fd_frag_meta_t * shred_mcache = shred_repair_link->mcache;
    void * shred_dcache = shred_repair_link->dcache;
    ulong shred_depth = shred_repair_link->depth;
    void * shred_wksp_laddr = config->topo.workspaces[ config->topo.objs[ shred_repair_link->dcache_obj_id ].wksp_id ].wksp;
    ulong shred_chunk0 = fd_dcache_compact_chunk0( shred_wksp_laddr, shred_dcache );
    ulong shred_chunk = shred_chunk0;

    if (FD_UNLIKELY( !shred_mcache || !shred_dcache || !shred_wksp_laddr || !shred_chunk0 )) {
      FD_LOG_ERR(( "Shred repair link not properly initialized"));
    }

    // Gossip peer logic now handled by rprtst tile

    // Create, populate and publish fake turbine slot data to shred_repair link
    forge_turbine_slot( args->repair_test.end_slot, shred_wksp_laddr, shred_chunk, shred_mcache, shred_depth );

    for(;;) pause();
   }

   action_t fd_action_repair_test = {
     .name = "repair_test",
     .args = repair_test_cmd_args,
     .fn   = repair_test_cmd_fn,
     .perm = dev_cmd_perm,
   };
