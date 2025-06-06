/* _GNU_SOURCE for recvmmsg and sendmmsg */
#define _GNU_SOURCE

#include "../../../../disco/topo/fd_topo.h"
#include "../../../../waltz/quic/fd_quic.h"
#include "../../../../waltz/tls/test_tls_helper.h"

#include <errno.h>
#include <linux/unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>

#include <stdio.h>
#include <stdlib.h>

#include <time.h>

/* max number of buffers batched for receive */
#define IO_VEC_CNT 128


struct signer_ctx {
  fd_sha512_t sha512[ 1 ];

  uchar public_key[ 32UL ];
  uchar private_key[ 32UL ];
};
typedef struct signer_ctx signer_ctx_t;


static void
signer( void *        _ctx,
        uchar         signature[ static 64 ],
        uchar const   payload[ static 130 ] ) {
  fd_tls_test_sign_ctx_t * ctx = (fd_tls_test_sign_ctx_t *)_ctx;
  fd_ed25519_sign( signature, payload, 130UL, ctx->public_key, ctx->private_key, ctx->sha512 );
}

static FD_FN_UNUSED
signer_ctx_t
signer_ctx( fd_rng_t * rng ) {
  signer_ctx_t ctx[1];
  FD_TEST( fd_sha512_join( fd_sha512_new( ctx->sha512 ) ) );
  for( ulong b=0; b<32UL; b++ ) ctx->private_key[b] = fd_rng_uchar( rng );
  fd_ed25519_public_from_private( ctx->public_key, ctx->private_key, ctx->sha512 );

  return *ctx;
}

static int
quic_tx_aio_send( void *                    _ctx,
                  fd_aio_pkt_info_t const * batch,
                  ulong                     batch_cnt,
                  ulong *                   opt_batch_idx,
                  int                       flush );


/* quic_now is called by the QUIC engine to get the current timestamp in
   UNIX time.  */

static ulong
quic_now( void * ctx ) {
  (void)ctx;
  return (ulong)fd_log_wallclock();
}

typedef struct {
  ulong round_robin_cnt;
  ulong round_robin_id;

  ulong packet_cnt;

  ulong         conn_cnt;
  int           conn_fd[ 128UL ];
  struct pollfd poll_fd[ 128UL ];

  signer_ctx_t     signer_ctx;
  int              no_quic;
  fd_quic_t *      quic;
  ushort           quic_port;
  fd_quic_conn_t * quic_conn;
  ulong            no_stream;
  uint             service_ratio_idx;

  // vector receive members
  struct mmsghdr rx_msgs[IO_VEC_CNT];
  struct mmsghdr tx_msgs[IO_VEC_CNT];
  struct iovec   rx_iovecs[IO_VEC_CNT];
  struct iovec   tx_iovecs[IO_VEC_CNT];
  uchar          rx_bufs[IO_VEC_CNT][2048];
  uchar          tx_bufs[IO_VEC_CNT][2048];

  ulong tx_idx;

  fd_quic_stream_t * stream;

  fd_wksp_t * mem;
} fd_benchs_ctx_t;

void
service_quic( fd_benchs_ctx_t * ctx ) {

  if( !ctx->no_quic ) {
    /* Publishes to mcache via callbacks */

    /* receive from socket, and pass to quic */
    int poll_rc = poll( ctx->poll_fd, ctx->conn_cnt, 0 );
    if( FD_LIKELY( poll_rc == 0 ) ) {
      return;
    } if( FD_UNLIKELY( poll_rc == -1 ) ) {
      if( FD_UNLIKELY( errno == EINTR ) ) return; /* will try later */
      FD_LOG_ERR(( "Error occurred during poll: %d %s", errno,
            strerror( errno ) ));
    }

    for( ulong j = 0; j < ctx->conn_cnt; ++j ) {
      int revents = ctx->poll_fd[j].revents;
      if( FD_LIKELY( revents & POLLIN ) ) {
        /* data available - receive up to IO_VEC_CNT buffers */
        struct timespec timeout = {0};
        int retval = recvmmsg( ctx->poll_fd[j].fd, ctx->rx_msgs, IO_VEC_CNT, 0, &timeout );
        if( FD_UNLIKELY( retval < 0 ) ) {
          FD_LOG_ERR(( "Error occurred on recvmmsg: %d %s", errno, strerror( errno ) ));
        }
        /* pass buffers to QUIC */
        for( ulong k = 0; k < (ulong)retval; k++ ) {
          uchar * buf = ctx->rx_bufs[k];

          /* set some required values */
          uint payload_len = ctx->rx_msgs[k].msg_len;
          uint udp_len     = payload_len + 8;
          uint ip_len      = udp_len + 20;

          /* set ver and len */
          buf[0] = 0x45;

          /* set protocol */
          buf[9] = 17;

          /* set udp length */
          buf[20 + 4] = (uchar)( udp_len >> 8 );
          buf[20 + 5] = (uchar)( udp_len      );

          /* set ip length */
          buf[2] = (uchar)( ip_len >> 8 );
          buf[3] = (uchar)( ip_len      );

          fd_quic_process_packet( ctx->quic, buf, ip_len );
        }
      } else if( FD_UNLIKELY( revents & POLLERR ) ) {
        int error = 0;
        socklen_t errlen = sizeof(error);

        if( getsockopt( ctx->poll_fd[j].fd, SOL_SOCKET, SO_ERROR, (void *)&error, &errlen ) == -1 ) {
          FD_LOG_ERR(( "Unknown error on socket" ));
        } else {
          FD_LOG_ERR(( "Error on socket: %d %s", error, strerror( error ) ));
        }
      }
    }
  }
}

static void
quic_stream_notify( fd_quic_stream_t * stream,
                    void *             stream_ctx,
                    int                type ) {
  (void)type;

  fd_benchs_ctx_t * ctx = (fd_benchs_ctx_t*)stream_ctx;
  if( FD_LIKELY( ctx ) ) {
    if( FD_LIKELY( ctx->stream == stream ) ) {
      ctx->stream = NULL;
    }
  } else {
    FD_LOG_ERR(( "quic_stream_notify - no context" ));
  }
}

/* quic_conn_new is invoked by the QUIC engine whenever a new connection
   is being established. */
static void
quic_conn_new( fd_quic_conn_t * conn,
               void *           _ctx ) {
  (void)conn;
  (void)_ctx;
}


void
handshake_complete( fd_quic_conn_t * conn,
                    void *           _ctx ) {
  (void)conn;
  (void)_ctx;
  FD_LOG_NOTICE(( "client handshake complete" ));
}


static void
quic_stream_notify( fd_quic_stream_t * stream,
                    void *             stream_ctx,
                    int                type );

static void
conn_final( fd_quic_conn_t * conn,
            void *           _ctx ) {
  (void)conn;

  fd_benchs_ctx_t * ctx = (fd_benchs_ctx_t *)_ctx;

  if( ctx ) {
    ctx->quic_conn = NULL;
    ctx->stream    = NULL;
  }
}

FD_FN_CONST static inline ulong
scratch_align( void ) {
  return fd_ulong_max( fd_quic_align(), alignof( fd_benchs_ctx_t ) );
}

void
populate_quic_limits( fd_quic_limits_t * limits ) {
  //int    argc = 0;
  //char * args[] = { NULL };
  //char ** argv = args;
  //fd_quic_limits_from_env( &argc, &argv, limits );
  limits->conn_cnt = 2;
  limits->handshake_cnt = limits->conn_cnt;
  limits->conn_id_cnt = 16;
  limits->inflight_frame_cnt = 1500;
  limits->tx_buf_sz = FD_TXN_MTU;
  limits->stream_pool_cnt = 1UL<<16;
  limits->stream_id_cnt = 1UL<<16;
}

void
populate_quic_config( fd_quic_config_t * config ) {
  config->role = FD_QUIC_ROLE_CLIENT;
  config->retry = 0;
  config->initial_rx_max_stream_data = 0; /* we don't expect the server to initiate streams */
  config->net.dscp = 0;
}

static inline ulong
scratch_footprint( fd_topo_tile_t const * tile ) {
  ulong l = FD_LAYOUT_INIT;
  l = FD_LAYOUT_APPEND( l, alignof( fd_benchs_ctx_t ), sizeof( fd_benchs_ctx_t ) );
  if( !tile->benchs.no_quic ) {
    fd_quic_limits_t quic_limits = {0};
    populate_quic_limits( &quic_limits );
    ulong quic_fp = fd_quic_footprint( &quic_limits );
    l = FD_LAYOUT_APPEND( l, fd_quic_align(),          quic_fp );
    l = FD_LAYOUT_APPEND( l, fd_aio_align(),           fd_aio_footprint() );
  }
  return FD_LAYOUT_FINI( l, scratch_align() );
}

static inline int
before_frag( fd_benchs_ctx_t * ctx,
             ulong             in_idx,
             ulong             seq,
             ulong             sig ) {
  (void)in_idx;
  (void)sig;

  return (int)( (seq%ctx->round_robin_cnt)!=ctx->round_robin_id );
}

static inline void
during_frag( fd_benchs_ctx_t * ctx,
             ulong             in_idx FD_PARAM_UNUSED,
             ulong             seq    FD_PARAM_UNUSED,
             ulong             sig    FD_PARAM_UNUSED,
             ulong             chunk,
             ulong             sz,
             ulong             ctl    FD_PARAM_UNUSED ) {
  if( ctx->no_quic ) {

    if( FD_UNLIKELY( -1==send( ctx->conn_fd[ ctx->packet_cnt % ctx->conn_cnt ], fd_chunk_to_laddr( ctx->mem, chunk ), sz, 0 ) ) )
      FD_LOG_ERR(( "send() failed (%i-%s)", errno, fd_io_strerror( errno ) ));

    ctx->packet_cnt++;
  } else {
    /* allows to accumulate multiple transactions before creating a UDP datagram */
    /* make this configurable */
    if( FD_UNLIKELY( ctx->service_ratio_idx++ == 8 ) ) {
      ctx->service_ratio_idx = 0;
      service_quic( ctx );
      fd_quic_service( ctx->quic );
    }

    if( FD_UNLIKELY( !ctx->quic_conn ) ) {
      ctx->no_stream = 0;

      /* try to connect */
      uint   dest_ip   = 0;
      ushort dest_port = fd_ushort_bswap( ctx->quic_port );

      ctx->quic_conn = fd_quic_connect( ctx->quic, dest_ip, dest_port, 0U, 12000 );

      /* failed? try later */
      if( FD_UNLIKELY( !ctx->quic_conn ) ) {
        service_quic( ctx );
        fd_quic_service( ctx->quic );
        return;
      }

      FD_LOG_NOTICE(( "connection created on port %d", (int)dest_port ));

      /* set the context to point to the location
         of the quic_conn pointer
         this allows the notification to NULL the value when
         a connection dies */
      fd_quic_conn_set_context( ctx->quic_conn, ctx );

      service_quic( ctx );
      fd_quic_service( ctx->quic );

      /* conn and streams may be invalidated by fd_quic_service */

      return;
    }

    fd_quic_stream_t * stream = ctx->stream;
    if( FD_UNLIKELY( !stream ) ) {
      ctx->stream = stream = fd_quic_conn_new_stream( ctx->quic_conn );
      if( FD_LIKELY( stream ) ) {
        fd_quic_stream_set_context( stream, ctx );
      }
    }

    if( FD_UNLIKELY( !stream ) ) {
      ctx->no_stream++;
      service_quic( ctx );
      fd_quic_service( ctx->quic );

      /* conn and streams may be invalidated by fd_quic_service */

      return;
    } else {
      int fin = 1;
      int rtn = fd_quic_stream_send( stream, fd_chunk_to_laddr( ctx->mem, chunk ), sz, fin );
      ctx->packet_cnt++;

      if( FD_LIKELY( rtn == FD_QUIC_SUCCESS ) ) {
        /* after using, fetch a new stream */
        ctx->stream = stream = fd_quic_conn_new_stream( ctx->quic_conn );
        if( FD_LIKELY( stream ) ) {
          fd_quic_stream_set_context( stream, ctx );
        }
      } else if( FD_UNLIKELY( rtn == 0 ) ) {
        FD_LOG_NOTICE(( "fd_quic_stream_send returned zero" ));
      } else {
        /* this can happen dring handshaking */
        if( rtn != FD_QUIC_SEND_ERR_INVAL_CONN ) {
          FD_LOG_ERR(( "fd_quic_stream_send failed with: %d", rtn ));
        }
      }
    }
  }
}

static void
privileged_init( fd_topo_t *      topo,
                 fd_topo_tile_t * tile ) {
  void * scratch = fd_topo_obj_laddr( topo, tile->tile_obj_id );

  FD_SCRATCH_ALLOC_INIT( l, scratch );
  fd_benchs_ctx_t * ctx = FD_SCRATCH_ALLOC_APPEND( l, alignof( fd_benchs_ctx_t ), sizeof( fd_benchs_ctx_t ) );

  int no_quic = ctx->no_quic = tile->benchs.no_quic;

  if( !no_quic ) {
    fd_quic_limits_t quic_limits = {0};
    populate_quic_limits( &quic_limits );
    ulong quic_fp = fd_quic_footprint( &quic_limits );
    if( FD_UNLIKELY( !quic_fp ) ) FD_LOG_ERR(( "invalid QUIC parameters" ));
    void * quic_mem = FD_SCRATCH_ALLOC_APPEND( l, fd_quic_align(), quic_fp );
    fd_quic_t * quic = fd_quic_join( fd_quic_new( quic_mem, &quic_limits ) );

    populate_quic_config( &quic->config );

    /* Signer */
    fd_rng_t   _rng[1];
    uint       seed = 4242424242;
    fd_rng_t * rng  = fd_rng_join( fd_rng_new( _rng, seed, 0UL ) );

    ctx->signer_ctx = signer_ctx( rng );

    quic->config.sign_ctx = &ctx->signer_ctx;
    quic->config.sign     = signer;

    fd_memcpy( quic->config.identity_public_key, ctx->signer_ctx.public_key, 32UL );

    /* store the pointer to quic and quic_rx_aio for later use */
    ctx->quic      = quic;
    ctx->quic_conn = NULL;
    ctx->stream    = NULL;
    ctx->tx_idx    = 0UL;

    /* call wallclock so glibc loads VDSO, which requires calling mmap while
       privileged */
    fd_log_wallclock();
  }

  ushort port = 12000;

  ctx->conn_cnt = tile->benchs.conn_cnt;
  if( !no_quic ) ctx->conn_cnt = 1;
  FD_TEST( ctx->conn_cnt <=sizeof(ctx->conn_fd)/sizeof(*ctx->conn_fd) );
  ctx->quic_port = tile->benchs.send_to_port;
  for( ulong i=0UL; i<ctx->conn_cnt ; i++ ) {
    int conn_fd = socket( AF_INET, SOCK_DGRAM, 0 );
    if( FD_UNLIKELY( -1==conn_fd ) ) FD_LOG_ERR(( "socket() failed (%i-%s)", errno, fd_io_strerror( errno ) ));

    int recvbuff = 8<<20;

    // Set the buffer size
    if( setsockopt( conn_fd, SOL_SOCKET, SO_RCVBUF, &recvbuff, sizeof(recvbuff) ) < 0 ) {
	FD_LOG_ERR(( "Error setting receive buffer size. Error: %d %s", errno, strerror( errno ) ));
    }

    int sendbuff = 8<<20;
    if( setsockopt( conn_fd, SOL_SOCKET, SO_SNDBUF, &sendbuff, sizeof(sendbuff) ) < 0 ) {
	FD_LOG_ERR(( "Error setting transmit buffer size. Error: %d %s", errno, strerror( errno ) ));
    }

    ushort found_port = 0;
    for( ulong j=0UL; j<10UL; j++ ) {
      struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = fd_ushort_bswap( port ),
        .sin_addr.s_addr = fd_uint_bswap( INADDR_ANY ),
      };
      if( FD_UNLIKELY( -1!=bind( conn_fd, fd_type_pun( &addr ), sizeof(addr) ) ) ) {
        found_port = port;
        break;
      }
      if( FD_UNLIKELY( EADDRINUSE!=errno ) ) FD_LOG_ERR(( "bind() failed (%i-%s)", errno, fd_io_strerror( errno ) ) );
      port = (ushort)(port + ctx->conn_cnt); /* Make sure it round robins to the same tile index */
    }
    if( FD_UNLIKELY( !found_port ) ) FD_LOG_ERR(( "bind() failed to find a src port" ));

    struct sockaddr_in addr = {
      .sin_family = AF_INET,
      .sin_port = fd_ushort_bswap( tile->benchs.send_to_port ),
      .sin_addr.s_addr = tile->benchs.send_to_ip_addr,
    };
    if( FD_UNLIKELY( -1==connect( conn_fd, fd_type_pun( &addr ), sizeof(addr) ) ) ) FD_LOG_ERR(( "connect() failed (%i-%s)", errno, fd_io_strerror( errno ) ));

    ctx->conn_fd[ i ]      = conn_fd;
    if( !no_quic ) {
      ctx->poll_fd[i].fd     = conn_fd;
      ctx->poll_fd[i].events = POLLIN;
    }
    port++;
  }
}

static void
unprivileged_init( fd_topo_t *      topo,
                   fd_topo_tile_t * tile ) {
  void * scratch = fd_topo_obj_laddr( topo, tile->tile_obj_id );

  FD_SCRATCH_ALLOC_INIT( l, scratch );
  fd_benchs_ctx_t * ctx = FD_SCRATCH_ALLOC_APPEND( l, alignof( fd_benchs_ctx_t ), sizeof( fd_benchs_ctx_t ) );

  ctx->packet_cnt = 0UL;

  ctx->round_robin_id = tile->kind_id;
  ctx->round_robin_cnt = fd_topo_tile_name_cnt( topo, "benchs" );

  ctx->mem = topo->workspaces[ topo->objs[ topo->links[ tile->in_link_id[ 0UL ] ].dcache_obj_id ].wksp_id ].wksp;

  void * aio_mem  = NULL;

  if( !ctx->no_quic ) {
    fd_quic_limits_t quic_limits = {0};
    populate_quic_limits( &quic_limits );
    ulong quic_fp = fd_quic_footprint( &quic_limits );
    FD_SCRATCH_ALLOC_APPEND( l, fd_quic_align(), quic_fp );

    fd_quic_t * quic = ctx->quic;
    aio_mem = FD_SCRATCH_ALLOC_APPEND( l, fd_aio_align(), fd_aio_footprint() );
    fd_aio_t * quic_tx_aio = fd_aio_join( fd_aio_new( aio_mem, ctx, quic_tx_aio_send ) );

    if( FD_UNLIKELY( !quic_tx_aio ) ) FD_LOG_ERR(( "fd_aio_join failed" ));

    ulong quic_idle_timeout_millis = 10000;  /* idle timeout in milliseconds */
    quic->config.role                       = FD_QUIC_ROLE_CLIENT;
    quic->config.idle_timeout               = quic_idle_timeout_millis * 1000000UL;
    quic->config.initial_rx_max_stream_data = 0;
    quic->config.retry                      = 0; /* unused on clients */

    quic->cb.conn_new         = quic_conn_new;
    quic->cb.conn_hs_complete = handshake_complete;
    quic->cb.conn_final       = conn_final;
    quic->cb.stream_notify    = quic_stream_notify;
    quic->cb.now              = quic_now;
    quic->cb.now_ctx          = NULL;
    quic->cb.quic_ctx         = ctx;

    fd_quic_set_aio_net_tx( quic, quic_tx_aio );
    if( FD_UNLIKELY( !fd_quic_init( quic ) ) ) FD_LOG_ERR(( "fd_quic_init failed" ));

    ulong hdr_sz = 20 + 8;
    for( ulong i = 0; i < IO_VEC_CNT; i++ ) {
      /* leave space for headers */
      ctx->rx_iovecs[i].iov_base         = ctx->rx_bufs[i]         + hdr_sz;
      ctx->rx_iovecs[i].iov_len          = sizeof(ctx->rx_bufs[i]) - hdr_sz;
      ctx->rx_msgs[i].msg_hdr.msg_iov    = &ctx->rx_iovecs[i];
      ctx->rx_msgs[i].msg_hdr.msg_iovlen = 1;
    }
  }

  ulong scratch_top = FD_SCRATCH_ALLOC_FINI( l, 1UL );
  if( FD_UNLIKELY( scratch_top > (ulong)scratch + scratch_footprint( tile ) ) )
    FD_LOG_ERR(( "scratch overflow %lu %lu %lu", scratch_top - (ulong)scratch - scratch_footprint( tile ), scratch_top, (ulong)scratch + scratch_footprint( tile ) ));

}

static void
quic_tx_aio_send_flush( fd_benchs_ctx_t * ctx ) {
  if( FD_LIKELY( ctx->tx_idx ) ) {
    int flags = 0;
    int rtn = sendmmsg( ctx->conn_fd[0], ctx->tx_msgs, (uint)ctx->tx_idx, flags );
    if( FD_UNLIKELY( rtn < 0 ) ) {
      FD_LOG_NOTICE(( "Error occurred in sendmmsg. Error: %d %s",
          errno, strerror( errno ) ));
    }
    ctx->tx_idx = 0;
  }
}

static int
quic_tx_aio_send( void *                    _ctx,
                  fd_aio_pkt_info_t const * batch,
                  ulong                     batch_cnt,
                  ulong *                   opt_batch_idx,
                  int                       flush ) {
  fd_benchs_ctx_t * ctx = _ctx;

  /* quic adds ip and udp headers which we don't need */
  /* assume 20 + 8 for those */
  ulong hdr_sz = 20+8;

  if( FD_LIKELY( batch_cnt ) ) {
    /* do we have space? */
    ulong remain = IO_VEC_CNT - ctx->tx_idx;
    if( FD_UNLIKELY( remain > batch_cnt ) ) {
      quic_tx_aio_send_flush( ctx );

      /* tx_idx may have changed */
      remain = IO_VEC_CNT - ctx->tx_idx;
    }

    ulong cnt = fd_ulong_min( remain, batch_cnt );
    ulong tx_idx = ctx->tx_idx;
    for( ulong j = 0; j < cnt; ++j ) {
      if( FD_UNLIKELY( batch[j].buf_sz < hdr_sz ) ) continue;

      uchar * tx_buf = ctx->tx_bufs[tx_idx];

      /* copy, stripping the header */
      fd_memcpy( tx_buf, (uchar*)batch[j].buf + hdr_sz, batch[j].buf_sz - hdr_sz );

      ctx->tx_iovecs[tx_idx].iov_base         = tx_buf;
      ctx->tx_iovecs[tx_idx].iov_len          = batch[j].buf_sz - hdr_sz;
      ctx->tx_msgs[tx_idx].msg_hdr.msg_iov    = &ctx->tx_iovecs[tx_idx];
      ctx->tx_msgs[tx_idx].msg_hdr.msg_iovlen = 1;

      tx_idx++;
    }

    /* write back */
    ctx->tx_idx = tx_idx;

    // TODO count drops?
    // ctx->dropped += batch_cnt - remain;
  }

  if( FD_UNLIKELY( ctx->tx_idx == IO_VEC_CNT || flush ) ) {
    quic_tx_aio_send_flush( ctx );
  }

  if( FD_LIKELY( opt_batch_idx ) ) *opt_batch_idx = batch_cnt;

  return 0;
}

#define STEM_BURST (1UL)

#define STEM_CALLBACK_CONTEXT_TYPE  fd_benchs_ctx_t
#define STEM_CALLBACK_CONTEXT_ALIGN alignof(fd_benchs_ctx_t)

#define STEM_CALLBACK_BEFORE_FRAG before_frag
#define STEM_CALLBACK_DURING_FRAG during_frag

#include "../../../../disco/stem/fd_stem.c"

fd_topo_run_tile_t fd_tile_benchs = {
  .name                     = "benchs",
  .scratch_align            = scratch_align,
  .scratch_footprint        = scratch_footprint,
  .privileged_init          = privileged_init,
  .unprivileged_init        = unprivileged_init,
  .run                      = stem_run,
};
