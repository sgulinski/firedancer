#ifndef HEADER_fd_src_flamenco_runtime_tests_fd_exec_instr_test_h
#define HEADER_fd_src_flamenco_runtime_tests_fd_exec_instr_test_h

/* fd_exec_instr_test.h provides APIs for running instruction processor
   tests. */

#include "../../fd_flamenco.h"
#include "../../fd_flamenco_base.h"
#include "../fd_runtime.h"
#include "generated/elf.pb.h"
#include "generated/invoke.pb.h"
#include "generated/txn.pb.h"
#include "generated/vm.pb.h"
#include "generated/block.pb.h"
#include "../../../funk/fd_funk.h"
#include "../../vm/fd_vm.h"
#include "../../../ballet/murmur3/fd_murmur3.h"

/* fd_exec_instr_test_runner_t provides fake fd_exec_instr_ctx_t to
   test processing of individual instructions. */

struct fd_exec_instr_test_runner {
  fd_funk_t * funk;
  fd_spad_t * spad;
};
typedef struct fd_exec_instr_test_runner fd_exec_instr_test_runner_t;

FD_PROTOTYPES_BEGIN

/* Constructors */

ulong
fd_exec_instr_test_runner_align( void );

ulong
fd_exec_instr_test_runner_footprint( void );

/* fd_exec_instr_test_runner_new formats two memory regions, one for use as an
   instruction test runner and another for an spad.  `mem` must be
   part of an fd_wksp.  Does additional wksp allocs.  wksp_tag is the tag
   used for wksp allocs managed by the runner.  Returns newly created runner
   on success.  On failure, returns NULL and logs reason for error. */

fd_exec_instr_test_runner_t *
fd_exec_instr_test_runner_new( void * mem,
                               void * spad_mem,
                               ulong  wksp_tag );

/* fd_exec_instr_test_runner_delete frees wksp allocations managed by
   runner and returns the memory region backing runner itself back to
   the caller. */

void *
fd_exec_instr_test_runner_delete( fd_exec_instr_test_runner_t * runner );

fd_spad_t *
fd_exec_instr_test_runner_get_spad( fd_exec_instr_test_runner_t * runner );

/* fd_exec_test_instr_context_create takes in a test runner and InstrCtx protobuf
   and creates an fd_exec_instr_ctx_t that can be used in runtime.

   Setting is_syscall avoids some operations/checks only relevant for
   program instructions.

   Should be coupled with fd_exec_test_instr_context_destroy when the instr_ctx
   is no longer needed. */
int
fd_exec_test_instr_context_create( fd_exec_instr_test_runner_t *        runner,
                                   fd_exec_instr_ctx_t *                ctx,
                                   fd_exec_test_instr_context_t const * test_ctx,
                                   bool                                 is_syscall );

/* Frees an instr_ctx created by fd_exec_test_instr_context_create */
void
fd_exec_test_instr_context_destroy( fd_exec_instr_test_runner_t * runner,
                                    fd_exec_instr_ctx_t *         ctx );



/* User API */

/* fd_exec_instr_test_run executes a given instruction context (input)
   and returns the effects of executing that instruction to the caller.
   output_buf points to a memory region of output_bufsz bytes where the
   result is allocated into.  On successful execution, *output points
   to a newly created instruction effects object, and returns the number
   of bytes allocated at output_buf.  (The caller can use this to shrink
   the output buffer)  Note that an instruction that errored (in the
   runtime) is also considered a successful execution.  On failure to
   execute, returns 0UL and leaves *object undefined and logs reason
   for failure.  Reasons for failure include insufficient output_bufsz. */

ulong
fd_exec_instr_test_run( fd_exec_instr_test_runner_t * runner,
                        void const *                  input_,
                        void **                       output_,
                        void *                        output_buf,
                        ulong                         output_bufsz );

/*
   Similar to above, but executes a txn given txn context (input)
*/
ulong
fd_exec_txn_test_run( fd_exec_instr_test_runner_t * runner, // Runner only contains funk instance, so we can borrow instr test runner
                      void const *                  input_,
                      void **                       output_,
                      void *                        output_buf,
                      ulong                         output_bufsz );

/*
   Executes several transactions within a single slot. A few things to know when using this harness...
   - All sysvars must be provided
   - This does not test sigverify or POH
   - Epoch boundaries are tested
   - Tested Firedancer code is `fd_runtime_block_execute_tpool`
   - Associated entrypoint tested in Agave is `confirm_slot_entries` (except sigverify and verify_ticks are removed)
   - (idk about this yet) Recent blockhashes sysvar account must NOT be provided in the input account states. Instead, the sysvar is populated through the input blockhash queue.
*/
ulong
fd_exec_block_test_run( fd_exec_instr_test_runner_t * runner,
                        void const *                  input_,
                        void **                       output_,
                        void *                        output_buf,
                        ulong                         output_bufsz );

/* Loads an ELF binary (in input->elf.data()).
   output_buf points to a memory region of output_bufsz bytes where the
   result is allocated into. During execution, the contents of
   fd_sbpf_program_t are wrapped in *output (backed by output_buf).

   Returns number of bytes allocated at output_buf OR 0UL on any
   harness-specific failures. Execution failures still return number of allocated bytes,
   but output is incomplete/undefined.
   */
ulong
fd_sbpf_program_load_test_run( fd_exec_instr_test_runner_t * runner,
                               void const *                  input_,
                               void **                       output_,
                               void *                        output_buf,
                               ulong                         output_bufsz );

ulong
fd_exec_vm_syscall_test_run( fd_exec_instr_test_runner_t * runner,
                             void const *                  input_,
                             void **                       output_,
                             void *                        output_buf,
                             ulong                         output_bufsz );


FD_PROTOTYPES_END

#endif /* HEADER_fd_src_flamenco_runtime_tests_fd_exec_instr_test_h */
