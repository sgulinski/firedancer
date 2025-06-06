#if !FD_HAS_HOSTED
#error "This target requires FD_HAS_HOSTED"
#endif

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

#include "../../util/fd_util.h"
#include "../../util/sanitize/fd_fuzz.h"
#include "fd_ed25519.h"

int
LLVMFuzzerInitialize( int  *   argc,
                      char *** argv ) {
  /* Set up shell without signal handlers */
  putenv( "FD_LOG_BACKTRACE=0" );
  fd_boot( argc, argv );
  atexit( fd_halt );
  fd_log_level_core_set(3); /* crash on warning log */
  return 0;
}

struct verification_test {
  uchar sig[ 64 ];
  uchar pub[ 32 ];
  uchar msg[ ];
};
typedef struct verification_test verification_test_t;

/* This fuzzer tries to verify random data */

int
LLVMFuzzerTestOneInput( uchar const * data,
                        ulong         size ) {
  if( FD_UNLIKELY( size<96UL ) ) return -1;

  verification_test_t * const test = ( verification_test_t * const ) data;
  ulong sz = size-96UL;

  fd_sha512_t _sha[1];
  fd_sha512_t *sha = fd_sha512_join( fd_sha512_new( _sha ) );

  int result = fd_ed25519_verify( test->msg, sz, test->sig, test->pub, sha );
  assert( result != FD_ED25519_SUCCESS );

  FD_FUZZ_MUST_BE_COVERED;
  return 0;
}
