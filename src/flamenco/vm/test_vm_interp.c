#include "fd_vm.h"
#include "fd_vm_base.h"
#include "fd_vm_private.h"
#include "test_vm_util.h"
#include "../runtime/context/fd_exec_slot_ctx.h"
#include <stdlib.h>  /* malloc */

static int
accumulator_syscall( FD_PARAM_UNUSED void *  _vm,
                     /**/            ulong   arg0,
                     /**/            ulong   arg1,
                     /**/            ulong   arg2,
                     /**/            ulong   arg3,
                     /**/            ulong   arg4,
                     /**/            ulong * ret ) {
  *ret = arg0 + arg1 + arg2 + arg3 + arg4;
  return 0;
}

static void
test_program_success( char *                test_case_name,
                      ulong                 expected_result,
                      ulong const *         text,
                      ulong                 text_cnt,
                      fd_sbpf_syscalls_t *  syscalls,
                      fd_exec_instr_ctx_t * instr_ctx ) {
//FD_LOG_NOTICE(( "Test program: %s", test_case_name ));

  fd_sha256_t _sha[1];
  fd_sha256_t * sha = fd_sha256_join( fd_sha256_new( _sha ) );

  fd_vm_t _vm[1];
  fd_vm_t * vm = fd_vm_join( fd_vm_new( _vm ) );
  FD_TEST( vm );

  int vm_ok = !!fd_vm_init(
      /* vm                 */ vm,
      /* instr_ctx          */ instr_ctx,
      /* heap_max           */ FD_VM_HEAP_DEFAULT,
      /* entry_cu           */ FD_VM_COMPUTE_UNIT_LIMIT,
      /* rodata             */ (uchar *)text,
      /* rodata_sz          */ 8UL*text_cnt,
      /* text               */ text,
      /* text_cnt           */ text_cnt,
      /* text_off           */ 0UL,
      /* text_sz            */ 8UL*text_cnt,
      /* entry_pc           */ 0UL,
      /* calldests          */ NULL,
      /* sbpf_version       */ TEST_VM_DEFAULT_SBPF_VERSION,
      /* syscalls           */ syscalls,
      /* trace              */ NULL,
      /* sha                */ sha,
      /* mem_regions        */ NULL,
      /* mem_regions_cnt    */ 0UL,
      /* mem_regions_accs   */ NULL,
      /* is_deprecated      */ 0,
      /* direct mapping     */ FD_FEATURE_ACTIVE( instr_ctx->txn_ctx->slot, &instr_ctx->txn_ctx->features, bpf_account_data_direct_mapping ),
      /* dump_syscall_to_pb */ 0
  );
  FD_TEST( vm_ok );

  /* FIXME: GROSS */
  vm->pc        = vm->entry_pc;
  vm->ic        = 0UL;
  vm->cu        = vm->entry_cu;
  vm->frame_cnt = 0UL;
  vm->heap_sz   = 0UL;
  fd_vm_mem_cfg( vm );

  int err = fd_vm_validate( vm );
  if( FD_UNLIKELY( err ) ) FD_LOG_ERR(( "validation failed: %i-%s", err, fd_vm_strerror( err ) ));

  long dt = -fd_log_wallclock();
  err = fd_vm_exec( vm );
  dt += fd_log_wallclock();

  if( FD_UNLIKELY( vm->reg[0]!=expected_result ) ) {
    FD_LOG_WARNING(( "Interp err: %i (%s)",   err,        fd_vm_strerror( err ) ));
    FD_LOG_WARNING(( "RET:        %lu 0x%lx", vm->reg[0], vm->reg[0]            ));
    FD_LOG_WARNING(( "PC:         %lu 0x%lx", vm->pc,     vm->pc                ));
    FD_LOG_WARNING(( "IC:         %lu 0x%lx", vm->ic,     vm->ic                ));
  }
//FD_LOG_NOTICE(( "Instr counter: %lu", vm.ic ));
  FD_TEST( vm->reg[0]==expected_result );
  FD_LOG_NOTICE(( "%-20s %11li ns", test_case_name, dt ));
//FD_LOG_NOTICE(( "Time/Instr: %f ns", (double)dt / (double)vm.ic ));
//FD_LOG_NOTICE(( "Mega Instr/Sec: %f", 1000.0 * ((double)vm.ic / (double) dt)));
}

static void
generate_random_alu_instrs( fd_rng_t * rng,
                            ulong *    text,
                            ulong      text_cnt ) {
  static uchar const opcodes[25] = {
    FD_SBPF_OP_ADD_IMM,
    FD_SBPF_OP_ADD_REG,
    FD_SBPF_OP_SUB_IMM,
    FD_SBPF_OP_SUB_REG,
    FD_SBPF_OP_MUL_IMM,
    FD_SBPF_OP_MUL_REG,
    FD_SBPF_OP_DIV_IMM,
    FD_SBPF_OP_DIV_REG,
    FD_SBPF_OP_OR_IMM,
    FD_SBPF_OP_OR_REG,
    FD_SBPF_OP_AND_IMM,
    FD_SBPF_OP_AND_REG,
    FD_SBPF_OP_LSH_IMM,
    FD_SBPF_OP_LSH_REG,
    FD_SBPF_OP_RSH_IMM,
    FD_SBPF_OP_RSH_REG,
    FD_SBPF_OP_NEG,
    FD_SBPF_OP_MOD_IMM,
    FD_SBPF_OP_MOD_REG,
    FD_SBPF_OP_XOR_IMM,
    FD_SBPF_OP_XOR_REG,
    FD_SBPF_OP_MOV_IMM,
    FD_SBPF_OP_MOV_REG,
    FD_SBPF_OP_ARSH_IMM,
    FD_SBPF_OP_ARSH_REG,
  };

  if( FD_UNLIKELY( !text_cnt ) ) return;

  fd_sbpf_instr_t instr;
  for( ulong i=0UL; i<text_cnt-1UL; i++ ) {
    instr.opcode.raw = opcodes[fd_rng_ulong_roll(rng, 25)];
    instr.dst_reg    = (1+fd_rng_uchar_roll(rng, 9)) & 0xFUL;
    instr.src_reg    = (1+fd_rng_uchar_roll(rng, 9)) & 0xFUL;
    instr.offset     = 0;
    instr.imm        = fd_rng_uint_roll(rng, 1024*1024);
    switch( instr.opcode.raw ) {
    case 0x34:  /* FD_SBPF_OP_DIV_IMM */
    case 0x94:  /* FD_SBPF_OP_MOD_IMM */
      instr.imm = fd_uint_max( instr.imm, 1 );
      break;
    case 0x64:  /* FD_SBPF_OP_LSH_IMM */
    case 0x74:  /* FD_SBPF_OP_RSH_IMM */
    case 0xc4:  /* FD_SBPF_OP_ARSH_IMM */
      instr.imm &= 31;
      break;
    }
    text[i] = fd_sbpf_ulong( instr );
  }
  instr.opcode.raw = FD_SBPF_OP_EXIT;
  text[text_cnt-1UL] = fd_sbpf_ulong( instr );
}

static void
generate_random_alu64_instrs( fd_rng_t * rng,
                              ulong *    text,
                              ulong      text_cnt ) {

  static uchar const opcodes[25] = {
    FD_SBPF_OP_ADD64_IMM,
    FD_SBPF_OP_ADD64_REG,
    FD_SBPF_OP_SUB64_IMM,
    FD_SBPF_OP_SUB64_REG,
    FD_SBPF_OP_MUL64_IMM,
    FD_SBPF_OP_MUL64_REG,
    FD_SBPF_OP_DIV64_IMM,
    FD_SBPF_OP_DIV64_REG,
    FD_SBPF_OP_OR64_IMM,
    FD_SBPF_OP_OR64_REG,
    FD_SBPF_OP_AND64_IMM,
    FD_SBPF_OP_AND64_REG,
    FD_SBPF_OP_LSH64_IMM,
    FD_SBPF_OP_LSH64_REG,
    FD_SBPF_OP_RSH64_IMM,
    FD_SBPF_OP_RSH64_REG,
    FD_SBPF_OP_NEG64,
    FD_SBPF_OP_MOD64_IMM,
    FD_SBPF_OP_MOD64_REG,
    FD_SBPF_OP_XOR64_IMM,
    FD_SBPF_OP_XOR64_REG,
    FD_SBPF_OP_MOV64_IMM,
    FD_SBPF_OP_MOV64_REG,
    FD_SBPF_OP_ARSH64_IMM,
    FD_SBPF_OP_ARSH64_REG,
  };

  if( FD_UNLIKELY( !text_cnt ) ) return;

  fd_sbpf_instr_t instr;
  for( ulong i=0UL; i<text_cnt-1UL; i++ ) {
    instr.opcode.raw = opcodes[fd_rng_ulong_roll(rng, 25)];
    instr.dst_reg    = (1+fd_rng_uchar_roll(rng, 9)) & 0xFUL;
    instr.src_reg    = (1+fd_rng_uchar_roll(rng, 9)) & 0xFUL;
    instr.offset     = 0;
    instr.imm        = fd_rng_uint_roll( rng, 1024*1024 );
    switch( instr.opcode.raw ) {
    case 0x37:  /* FD_SBPF_OP_DIV64_IMM */
    case 0x97:  /* FD_SBPF_OP_MOD64_IMM */
      instr.imm = fd_uint_max( instr.imm, 1 );
      break;
    case 0x67:  /* FD_SBPF_OP_LSH_IMM */
    case 0x77:  /* FD_SBPF_OP_RSH_IMM */
    case 0xc7:  /* FD_SBPF_OP_ARSH_IMM */
      instr.imm &= 31;
      break;
    }
    text[i] = fd_sbpf_ulong( instr );
  }
  instr.opcode.raw = FD_SBPF_OP_EXIT;
  text[text_cnt-1UL] = fd_sbpf_ulong( instr );
}

/* test_0cu_exit ensures that the VM correctly exits the root frame if
   the CU count after the final exit instruction reaches zero. */

static void
test_0cu_exit( void ) {

  fd_sha256_t _sha[1];
  fd_sha256_t * sha = fd_sha256_join( fd_sha256_new( _sha ) );

  fd_vm_t _vm[1];
  fd_vm_t * vm = fd_vm_join( fd_vm_new( _vm ) );
  FD_TEST( vm );

  ulong const text[3] = {
    fd_vm_instr( FD_SBPF_OP_XOR64_REG, 0, 0, 0, 0 ),
    fd_vm_instr( FD_SBPF_OP_XOR64_REG, 0, 0, 0, 0 ),
    fd_vm_instr( FD_SBPF_OP_EXIT,      0, 0, 0, 0 )
  };
  ulong text_cnt = 3UL;

  fd_valloc_t valloc = fd_libc_alloc_virtual();
  fd_exec_slot_ctx_t  * slot_ctx  = fd_valloc_malloc( valloc, FD_EXEC_SLOT_CTX_ALIGN,    FD_EXEC_SLOT_CTX_FOOTPRINT );
  fd_exec_instr_ctx_t * instr_ctx = test_vm_minimal_exec_instr_ctx( valloc, slot_ctx );

  /* Ensure the VM exits with success if the CU count after the final
     exit instruction reaches zero. */

  int vm_ok = !!fd_vm_init(
      /* vm                 */ vm,
      /* instr_ctx          */ instr_ctx,
      /* heap_max           */ FD_VM_HEAP_DEFAULT,
      /* entry_cu           */ text_cnt,
      /* rodata             */ (uchar *)text,
      /* rodata_sz          */ 8UL*text_cnt,
      /* text               */ text,
      /* text_cnt           */ text_cnt,
      /* text_off           */ 0UL,
      /* text_sz            */ 8UL*text_cnt,
      /* entry_pc           */ 0UL,
      /* calldests          */ NULL,
      /* sbpf_version       */ TEST_VM_DEFAULT_SBPF_VERSION,
      /* syscalls           */ NULL,
      /* trace              */ NULL,
      /* sha                */ sha,
      /* mem_regions        */ NULL,
      /* mem_regions_cnt    */ 0UL,
      /* mem_regions_accs   */ NULL,
      /* is_deprecated      */ 0,
      /* direct mapping     */ FD_FEATURE_ACTIVE( instr_ctx->txn_ctx->slot, &instr_ctx->txn_ctx->features, bpf_account_data_direct_mapping ),
      /* dump_syscall_to_pb */ 0
  );
  FD_TEST( vm_ok );

  FD_TEST( fd_vm_validate( vm )==FD_VM_SUCCESS );
  FD_TEST( fd_vm_exec    ( vm )==FD_VM_SUCCESS );
  FD_TEST( vm->cu == 0UL );

  /* Ensure the VM exits with failure if CUs are exhausted. */

  vm_ok = !!fd_vm_init(
      /* vm                 */ vm,
      /* instr_ctx          */ instr_ctx,
      /* heap_max           */ FD_VM_HEAP_DEFAULT,
      /* entry_cu           */ text_cnt - 1UL,
      /* rodata             */ (uchar *)text,
      /* rodata_sz          */ 8UL*text_cnt,
      /* text               */ text,
      /* text_cnt           */ text_cnt,
      /* text_off           */ 0UL,
      /* text_sz            */ 8UL*text_cnt,
      /* entry_pc           */ 0UL,
      /* calldests          */ NULL,
      /* sbpf_version       */ TEST_VM_DEFAULT_SBPF_VERSION,
      /* syscalls           */ NULL,
      /* trace              */ NULL,
      /* sha                */ sha,
      /* mem_regions        */ NULL,
      /* mem_regions_cnt    */ 0UL,
      /* mem_regions_accs   */ NULL,
      /* is_deprecated      */ 0,
      /* direct mapping     */ FD_FEATURE_ACTIVE( instr_ctx->txn_ctx->slot, &instr_ctx->txn_ctx->features, bpf_account_data_direct_mapping ),
      /* dump_syscall_to_pb */ 0
  );
  FD_TEST( vm_ok );

  FD_TEST( fd_vm_validate( vm )==FD_VM_SUCCESS );
  FD_TEST( fd_vm_exec    ( vm )==FD_VM_ERR_EBPF_EXCEEDED_MAX_INSTRUCTIONS );

  fd_vm_delete( fd_vm_leave( vm ) );
  fd_valloc_free( valloc, slot_ctx );
  test_vm_exec_instr_ctx_delete( instr_ctx, fd_libc_alloc_virtual() );
  fd_sha256_delete( fd_sha256_leave( sha ) );
}

static const uint FD_VM_SBPF_STATIC_SYSCALLS_LIST[] = {
  0,
  //  1 = abort
  0xb6fc1a11,
  //  2 = sol_panic_
  0x686093bb,
  //  3 = sol_memcpy_
  0x717cc4a3,
  //  4 = sol_memmove_
  0x434371f8,
  //  5 = sol_memset_
  0x3770fb22,
  //  6 = sol_memcmp_
  0x5fdcde31,
  //  7 = sol_log_
  0x207559bd,
  //  8 = sol_log_64_
  0x5c2a3178,
  //  9 = sol_log_pubkey
  0x7ef088ca,
  // 10 = sol_log_compute_units_
  0x52ba5096,
  // 11 = sol_alloc_free_
  0x83f00e8f,
  // 12 = sol_invoke_signed_c
  0xa22b9c85,
  // 13 = sol_invoke_signed_rust
  0xd7449092,
  // 14 = sol_set_return_data
  0xa226d3eb,
  // 15 = sol_get_return_data
  0x5d2245e4,
  // 16 = sol_log_data
  0x7317b434,
  // 17 = sol_sha256
  0x11f49d86,
  // 18 = sol_keccak256
  0xd7793abb,
  // 19 = sol_secp256k1_recover
  0x17e40350,
  // 20 = sol_blake3
  0x174c5122,
  // 21 = sol_poseidon
  0xc4947c21,
  // 22 = sol_get_processed_sibling_instruction
  0xadb8efc8,
  // 23 = sol_get_stack_height
  0x85532d94,
  // 24 = sol_curve_validate_point
  0xaa2607ca,
  // 25 = sol_curve_group_op
  0xdd1c41a6,
  // 26 = sol_curve_multiscalar_mul
  0x60a40880,
  // 27 = sol_curve_pairing_map
  0xf111a47e,
  // 28 = sol_alt_bn128_group_op
  0xae0c318b,
  // 29 = sol_alt_bn128_compression
  0x334fd5ed,
  // 30 = sol_big_mod_exp
  0x780e4c15,
  // 31 = sol_remaining_compute_units
  0xedef5aee,
  // 32 = sol_create_program_address
  0x9377323c,
  // 33 = sol_try_find_program_address
  0x48504a38,
  // 34 = sol_get_sysvar
  0x13c1b505,
  // 35 = sol_get_epoch_stake
  0x5be92f4a,
  // 36 = sol_get_clock_sysvar
  0xd56b5fe9,
  // 37 = sol_get_epoch_schedule_sysvar
  0x23a29a61,
  // 38 = sol_get_last_restart_slot
  0x188a0031,
  // 39 = sol_get_epoch_rewards_sysvar
  0xfdba2b3b,
  // 40 = sol_get_fees_sysvar
  0x3b97b73c,
  // 41 = sol_get_rent_sysvar
  0xbf7188f6,
};
#define FD_VM_SBPF_STATIC_SYSCALLS_LIST_SZ (sizeof(FD_VM_SBPF_STATIC_SYSCALLS_LIST) / sizeof(uint))

static void
test_static_syscalls_list( void ) {
  const char *static_syscalls_from_simd[] = {
    "abort",
    "sol_panic_",
    "sol_memcpy_",
    "sol_memmove_",
    "sol_memset_",
    "sol_memcmp_",
    "sol_log_",
    "sol_log_64_",
    "sol_log_pubkey",
    "sol_log_compute_units_",
    "sol_alloc_free_",
    "sol_invoke_signed_c",
    "sol_invoke_signed_rust",
    "sol_set_return_data",
    "sol_get_return_data",
    "sol_log_data",
    "sol_sha256",
    "sol_keccak256",
    "sol_secp256k1_recover",
    "sol_blake3",
    "sol_poseidon",
    "sol_get_processed_sibling_instruction",
    "sol_get_stack_height",
    "sol_curve_validate_point",
    "sol_curve_group_op",
    "sol_curve_multiscalar_mul",
    "sol_curve_pairing_map",
    "sol_alt_bn128_group_op",
    "sol_alt_bn128_compression",
    "sol_big_mod_exp",
    "sol_remaining_compute_units",
    "sol_create_program_address",
    "sol_try_find_program_address",
    "sol_get_sysvar",
    "sol_get_epoch_stake",
    "sol_get_clock_sysvar",
    "sol_get_epoch_schedule_sysvar",
    "sol_get_last_restart_slot",
    "sol_get_epoch_rewards_sysvar",
    "sol_get_fees_sysvar",
    "sol_get_rent_sysvar",
  };

  FD_TEST( FD_VM_SBPF_STATIC_SYSCALLS_LIST[0]==0 );
  for( ulong i=1; i<FD_VM_SBPF_STATIC_SYSCALLS_LIST_SZ; i++ ) {
    const char *name = static_syscalls_from_simd[i-1];
    uint key = fd_murmur3_32( name, strlen(name), 0 );
    FD_TEST( FD_VM_SBPF_STATIC_SYSCALLS_LIST[i]==key );
  }
}

static fd_sbpf_syscalls_t _syscalls[ FD_SBPF_SYSCALLS_SLOT_CNT ];

int
main( int     argc,
      char ** argv ) {
  fd_boot( &argc, &argv );

  fd_rng_t _rng[1]; fd_rng_t * rng = fd_rng_join( fd_rng_new( _rng, 0U, 0UL ) );

  fd_sbpf_syscalls_t * syscalls = fd_sbpf_syscalls_join( fd_sbpf_syscalls_new( _syscalls ) ); FD_TEST( syscalls );

  fd_valloc_t valloc = fd_libc_alloc_virtual();
  fd_exec_slot_ctx_t  * slot_ctx  = fd_valloc_malloc( valloc, FD_EXEC_SLOT_CTX_ALIGN,    FD_EXEC_SLOT_CTX_FOOTPRINT );
  fd_exec_instr_ctx_t * instr_ctx = test_vm_minimal_exec_instr_ctx( valloc, slot_ctx );

  FD_TEST( fd_vm_syscall_register( syscalls, "accumulator", accumulator_syscall )==FD_VM_SUCCESS );

# define TEST_PROGRAM_SUCCESS( test_case_name, expected_result, text_cnt, ... ) do { \
    ulong _text[ text_cnt ] = { __VA_ARGS__ };                                       \
    test_program_success( (test_case_name), (expected_result), _text, (text_cnt), syscalls, instr_ctx ); \
  } while(0)

# define FD_SBPF_INSTR(op, dst, src, off, val) (fd_vm_instr( op, dst, src, off, val ))

  TEST_PROGRAM_SUCCESS("add", 0x3, 5,
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R0,  0,      0, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R1,  0,      0, 2),
    FD_SBPF_INSTR(FD_SBPF_OP_ADD_IMM,   FD_SBPF_R0,  0,      0, 1),
    FD_SBPF_INSTR(FD_SBPF_OP_ADD_REG,   FD_SBPF_R0,  FD_SBPF_R1,  0, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
  );

  TEST_PROGRAM_SUCCESS("add64", 0x3, 5,
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R0,  0,      0, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R1,  0,      0, 2),
    FD_SBPF_INSTR(FD_SBPF_OP_ADD64_IMM, FD_SBPF_R0,  0,      0, 1),
    FD_SBPF_INSTR(FD_SBPF_OP_ADD64_REG, FD_SBPF_R0,  FD_SBPF_R1,  0, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
  );

  TEST_PROGRAM_SUCCESS("alu-arith", 0x150, 17,
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R0,  0,      0, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R1,  0,      0, 1),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R2,  0,      0, 2),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R3,  0,      0, 3),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R4,  0,      0, 4),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R5,  0,      0, 5),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R6,  0,      0, 6),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R7,  0,      0, 7),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R8,  0,      0, 8),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R9,  0,      0, 9),

    FD_SBPF_INSTR(FD_SBPF_OP_ADD_IMM,   FD_SBPF_R0,  0,      0, 23),
    FD_SBPF_INSTR(FD_SBPF_OP_ADD_REG,   FD_SBPF_R0,  FD_SBPF_R7,  0, 0),

    FD_SBPF_INSTR(FD_SBPF_OP_SUB_IMM,   FD_SBPF_R0,  0,      0, 13),
    FD_SBPF_INSTR(FD_SBPF_OP_SUB_REG,   FD_SBPF_R0,  FD_SBPF_R1,  0, 0),

    FD_SBPF_INSTR(FD_SBPF_OP_MUL_IMM,   FD_SBPF_R0,  0,      0, 7),
    FD_SBPF_INSTR(FD_SBPF_OP_MUL_REG,   FD_SBPF_R0,  FD_SBPF_R3,  0, 0),

    /* Divide by zero faults */
    //FD_SBPF_INSTR(FD_SBPF_OP_DIV_IMM,   FD_SBPF_R0,  0,      0, 2),
    //FD_SBPF_INSTR(FD_SBPF_OP_DIV_REG,   FD_SBPF_R0,  FD_SBPF_R4,  0, 0),

    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
  );

  TEST_PROGRAM_SUCCESS("alu-bitwise", 0x11, 21,
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R0,  0,      0, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R1,  0,      0, 1),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R2,  0,      0, 2),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R3,  0,      0, 3),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R4,  0,      0, 4),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R5,  0,      0, 5),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R6,  0,      0, 6),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R7,  0,      0, 7),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R8,  0,      0, 8),

    FD_SBPF_INSTR(FD_SBPF_OP_OR_REG,    FD_SBPF_R0,  FD_SBPF_R5,  0, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_OR_IMM,    FD_SBPF_R0,  0,      0, 0xa0),

    FD_SBPF_INSTR(FD_SBPF_OP_AND_IMM,   FD_SBPF_R0,  0,      0, 0xa3),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R9,  0,      0, 0x91),
    FD_SBPF_INSTR(FD_SBPF_OP_AND_REG,   FD_SBPF_R0,  FD_SBPF_R9,  0, 0),

    FD_SBPF_INSTR(FD_SBPF_OP_LSH_IMM,   FD_SBPF_R0,  0,      0, 22),
    FD_SBPF_INSTR(FD_SBPF_OP_LSH_REG,   FD_SBPF_R0,  FD_SBPF_R8,  0, 0),

    FD_SBPF_INSTR(FD_SBPF_OP_RSH_IMM,   FD_SBPF_R0,  0,      0, 19),
    FD_SBPF_INSTR(FD_SBPF_OP_RSH_REG,   FD_SBPF_R0,  FD_SBPF_R7,  0, 0),

    FD_SBPF_INSTR(FD_SBPF_OP_XOR_IMM,   FD_SBPF_R0,  0,      0, 0x03),
    FD_SBPF_INSTR(FD_SBPF_OP_XOR_REG,   FD_SBPF_R0,  FD_SBPF_R2,  0, 0),

    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
  );

  TEST_PROGRAM_SUCCESS("alu64-arith", 0x2a, 19,
    FD_SBPF_INSTR(FD_SBPF_OP_MOV64_IMM,   FD_SBPF_R0,  0,      0, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV64_IMM,   FD_SBPF_R1,  0,      0, 1),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV64_IMM,   FD_SBPF_R2,  0,      0, 2),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV64_IMM,   FD_SBPF_R3,  0,      0, 3),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV64_IMM,   FD_SBPF_R4,  0,      0, 4),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV64_IMM,   FD_SBPF_R5,  0,      0, 5),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV64_IMM,   FD_SBPF_R6,  0,      0, 6),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV64_IMM,   FD_SBPF_R7,  0,      0, 7),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV64_IMM,   FD_SBPF_R8,  0,      0, 8),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV64_IMM,   FD_SBPF_R9,  0,      0, 9),

    FD_SBPF_INSTR(FD_SBPF_OP_ADD64_IMM,   FD_SBPF_R0,  0,           0, 23),
    FD_SBPF_INSTR(FD_SBPF_OP_ADD64_REG,   FD_SBPF_R0,  FD_SBPF_R7,  0, 0),

    FD_SBPF_INSTR(FD_SBPF_OP_SUB64_IMM,   FD_SBPF_R0,  0,           0, 13),
    FD_SBPF_INSTR(FD_SBPF_OP_SUB64_REG,   FD_SBPF_R0,  FD_SBPF_R1,  0, 0),

    FD_SBPF_INSTR(FD_SBPF_OP_MUL64_IMM,   FD_SBPF_R0,  0,           0, 7),
    FD_SBPF_INSTR(FD_SBPF_OP_MUL64_REG,   FD_SBPF_R0,  FD_SBPF_R3,  0, 0),

    FD_SBPF_INSTR(FD_SBPF_OP_DIV64_IMM,   FD_SBPF_R0,  0,           0, 2),
    FD_SBPF_INSTR(FD_SBPF_OP_DIV64_REG,   FD_SBPF_R0,  FD_SBPF_R4,  0, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
  );

  TEST_PROGRAM_SUCCESS("alu64-bitwise", 0x811, 21,
    FD_SBPF_INSTR(FD_SBPF_OP_MOV64_IMM,   FD_SBPF_R0,  0,      0, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV64_IMM,   FD_SBPF_R1,  0,      0, 1),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV64_IMM,   FD_SBPF_R2,  0,      0, 2),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV64_IMM,   FD_SBPF_R3,  0,      0, 3),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV64_IMM,   FD_SBPF_R4,  0,      0, 4),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV64_IMM,   FD_SBPF_R5,  0,      0, 5),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV64_IMM,   FD_SBPF_R6,  0,      0, 6),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV64_IMM,   FD_SBPF_R7,  0,      0, 7),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV64_IMM,   FD_SBPF_R8,  0,      0, 8),

    FD_SBPF_INSTR(FD_SBPF_OP_OR64_REG,    FD_SBPF_R0,  FD_SBPF_R5,  0, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_OR64_IMM,    FD_SBPF_R0,  0,      0, 0xa0),

    FD_SBPF_INSTR(FD_SBPF_OP_AND64_IMM,   FD_SBPF_R0,  0,      0, 0xa3),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV64_IMM,   FD_SBPF_R9,  0,      0, 0x91),
    FD_SBPF_INSTR(FD_SBPF_OP_AND64_REG,   FD_SBPF_R0,  FD_SBPF_R9,  0, 0),

    FD_SBPF_INSTR(FD_SBPF_OP_LSH64_IMM,   FD_SBPF_R0,  0,      0, 22),
    FD_SBPF_INSTR(FD_SBPF_OP_LSH64_REG,   FD_SBPF_R0,  FD_SBPF_R8,  0, 0),

    FD_SBPF_INSTR(FD_SBPF_OP_RSH64_IMM,   FD_SBPF_R0,  0,      0, 19),
    FD_SBPF_INSTR(FD_SBPF_OP_RSH64_REG,   FD_SBPF_R0,  FD_SBPF_R7,  0, 0),

    FD_SBPF_INSTR(FD_SBPF_OP_XOR64_IMM,   FD_SBPF_R0,  0,      0, 0x03),
    FD_SBPF_INSTR(FD_SBPF_OP_XOR64_REG,   FD_SBPF_R0,  FD_SBPF_R2,  0, 0),

    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
  );

  TEST_PROGRAM_SUCCESS("arsh-reg", 0xffff8000, 5,
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R0,  0,      0, 0xf8),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R1,  0,      0, 16),
    FD_SBPF_INSTR(FD_SBPF_OP_LSH_IMM,   FD_SBPF_R0,  0,      0, 28),
    FD_SBPF_INSTR(FD_SBPF_OP_ARSH_REG,  FD_SBPF_R0,  FD_SBPF_R1,  0, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
  );

  TEST_PROGRAM_SUCCESS("arsh", 0xffff8000, 4,
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R0,  0,      0, 0xf8),
    FD_SBPF_INSTR(FD_SBPF_OP_LSH_IMM,   FD_SBPF_R0,  0,      0, 28),
    FD_SBPF_INSTR(FD_SBPF_OP_ARSH_IMM,  FD_SBPF_R0,  0,      0, 16),
    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
  );

  TEST_PROGRAM_SUCCESS("arsh-high-shift", 0x4, 5,
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R0,  0,      0, 0x8),
    FD_SBPF_INSTR(FD_SBPF_OP_LDDW,      FD_SBPF_R1,  0,      0, 0x1),
    FD_SBPF_INSTR(FD_SBPF_OP_ADDL_IMM,  0,      0,      0, 0x1),
    FD_SBPF_INSTR(FD_SBPF_OP_ARSH_REG,  FD_SBPF_R0,  FD_SBPF_R1,  0, 16),
    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
  );

  TEST_PROGRAM_SUCCESS("arsh64", 0xfffffffffffffff8, 6,
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,     FD_SBPF_R0,  0,      0, 1),
    FD_SBPF_INSTR(FD_SBPF_OP_LSH64_IMM,   FD_SBPF_R0,  0,      0, 63),
    FD_SBPF_INSTR(FD_SBPF_OP_ARSH64_IMM,  FD_SBPF_R0,  0,      0, 55),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,     FD_SBPF_R1,  0,      0, 5),
    FD_SBPF_INSTR(FD_SBPF_OP_ARSH64_REG,  FD_SBPF_R0,  FD_SBPF_R1,  0, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,        0,      0,      0, 0),
  );

  TEST_PROGRAM_SUCCESS("be16-high", 0x1122, 4,
    FD_SBPF_INSTR(FD_SBPF_OP_LDDW,      FD_SBPF_R0,  0,      0, 0x44332211),
    FD_SBPF_INSTR(FD_SBPF_OP_ADDL_IMM,  0,      0,      0, 0x88776655),
    FD_SBPF_INSTR(FD_SBPF_OP_END_BE,    FD_SBPF_R0,  0,      0, 16),
    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
  );

  TEST_PROGRAM_SUCCESS("be16", 0x1122, 3,
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R0,  0,      0, 0x00002211),
    FD_SBPF_INSTR(FD_SBPF_OP_END_BE,    FD_SBPF_R0,  0,      0, 16),
    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
  );

  TEST_PROGRAM_SUCCESS("be32-high", 0x11223344, 4,
    FD_SBPF_INSTR(FD_SBPF_OP_LDDW,      FD_SBPF_R0,  0,      0, 0x44332211),
    FD_SBPF_INSTR(FD_SBPF_OP_ADDL_IMM,  0,      0,      0, 0x88776655),
    FD_SBPF_INSTR(FD_SBPF_OP_END_BE,    FD_SBPF_R0,  0,      0, 32),
    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
  );

  TEST_PROGRAM_SUCCESS("be32", 0x11223344, 3,
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R0,  0,      0, 0x44332211),
    FD_SBPF_INSTR(FD_SBPF_OP_END_BE,    FD_SBPF_R0,  0,      0, 32),
    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
  );

  TEST_PROGRAM_SUCCESS("be64", 0x1122334455667788, 4,
    FD_SBPF_INSTR(FD_SBPF_OP_LDDW,      FD_SBPF_R0,  0,      0, 0x44332211),
    FD_SBPF_INSTR(FD_SBPF_OP_ADDL_IMM,  0,      0,      0, 0x88776655),
    FD_SBPF_INSTR(FD_SBPF_OP_END_BE,    FD_SBPF_R0,  0,      0, 64),
    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
  );

  TEST_PROGRAM_SUCCESS("div-high-divisor", 0x3, 5,
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R0,  0,      0, 12),
    FD_SBPF_INSTR(FD_SBPF_OP_LDDW,      FD_SBPF_R1,  0,      0, 0x4),
    FD_SBPF_INSTR(FD_SBPF_OP_ADDL_IMM,  0,      0,      0, 0x1),
    FD_SBPF_INSTR(FD_SBPF_OP_DIV_REG,   FD_SBPF_R0,  FD_SBPF_R1,  0, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
  );

  TEST_PROGRAM_SUCCESS("div-imm", 0x3, 4,
    FD_SBPF_INSTR(FD_SBPF_OP_LDDW,      FD_SBPF_R0,  0,      0, 0xc),
    FD_SBPF_INSTR(FD_SBPF_OP_ADDL_IMM,  0,      0,      0, 0x1),
    FD_SBPF_INSTR(FD_SBPF_OP_DIV_IMM,   FD_SBPF_R0,  0,      0, 4),
    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
  );

  TEST_PROGRAM_SUCCESS("div-reg", 0x3, 5,
    FD_SBPF_INSTR(FD_SBPF_OP_LDDW,      FD_SBPF_R0,  0,      0, 0xc),
    FD_SBPF_INSTR(FD_SBPF_OP_ADDL_IMM,  0,      0,      0, 0x1),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R1,  0,      0, 4),
    FD_SBPF_INSTR(FD_SBPF_OP_DIV_REG,   FD_SBPF_R0,  FD_SBPF_R1,  0, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
  );

  TEST_PROGRAM_SUCCESS("div64-high-divisor", 0x15555555, 6,
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R0,  0,      0, 12),
    FD_SBPF_INSTR(FD_SBPF_OP_LDDW,      FD_SBPF_R1,  0,      0, 0x4),
    FD_SBPF_INSTR(FD_SBPF_OP_ADDL_IMM,  0,      0,      0, 0x1),
    FD_SBPF_INSTR(FD_SBPF_OP_DIV64_REG, FD_SBPF_R1,  FD_SBPF_R0,  0, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_REG,   FD_SBPF_R0,  FD_SBPF_R1,  0, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
  );

  TEST_PROGRAM_SUCCESS("div64-imm", 0x40000003, 4,
    FD_SBPF_INSTR(FD_SBPF_OP_LDDW,      FD_SBPF_R0,  0,      0, 0xc),
    FD_SBPF_INSTR(FD_SBPF_OP_ADDL_IMM,  0,      0,      0, 0x1),
    FD_SBPF_INSTR(FD_SBPF_OP_DIV64_IMM, FD_SBPF_R0,  0,      0, 4),
    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
  );

  TEST_PROGRAM_SUCCESS("div64-reg", 0x40000003, 6,
    FD_SBPF_INSTR(FD_SBPF_OP_LDDW,      FD_SBPF_R1,  0,      0, 0xc),
    FD_SBPF_INSTR(FD_SBPF_OP_ADDL_IMM,  0,      0,      0, 0x1),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R0,  0,      0, 4),
    FD_SBPF_INSTR(FD_SBPF_OP_DIV64_REG, FD_SBPF_R1,  FD_SBPF_R0,  0, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_REG,   FD_SBPF_R0,  FD_SBPF_R1,  0, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
  );

  TEST_PROGRAM_SUCCESS("mod-high-divisor", 0x0, 5,
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R0,  0,      0, 12),
    FD_SBPF_INSTR(FD_SBPF_OP_LDDW,      FD_SBPF_R1,  0,      0, 0x4),
    FD_SBPF_INSTR(FD_SBPF_OP_ADDL_IMM,  0,      0,      0, 0x1),
    FD_SBPF_INSTR(FD_SBPF_OP_MOD_REG,   FD_SBPF_R0,  FD_SBPF_R1,  0, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
  );

  TEST_PROGRAM_SUCCESS("mod-imm", 0x0, 4,
    FD_SBPF_INSTR(FD_SBPF_OP_LDDW,      FD_SBPF_R0,  0,      0, 0xc),
    FD_SBPF_INSTR(FD_SBPF_OP_ADDL_IMM,  0,      0,      0, 0x1),
    FD_SBPF_INSTR(FD_SBPF_OP_MOD_IMM,   FD_SBPF_R0,  0,      0, 4),
    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
  );

  TEST_PROGRAM_SUCCESS("mod-reg", 0x0, 5,
    FD_SBPF_INSTR(FD_SBPF_OP_LDDW,      FD_SBPF_R0,  0,      0, 0xc),
    FD_SBPF_INSTR(FD_SBPF_OP_ADDL_IMM,  0,      0,      0, 0x1),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R1,  0,      0, 4),
    FD_SBPF_INSTR(FD_SBPF_OP_MOD_REG,   FD_SBPF_R0,  FD_SBPF_R1,  0, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
  );

  TEST_PROGRAM_SUCCESS("mod64-high-divisor", 0x8, 6,
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R0,  0,      0, 12),
    FD_SBPF_INSTR(FD_SBPF_OP_LDDW,      FD_SBPF_R1,  0,      0, 0x4),
    FD_SBPF_INSTR(FD_SBPF_OP_ADDL_IMM,  0,      0,      0, 0x1),
    FD_SBPF_INSTR(FD_SBPF_OP_MOD64_REG, FD_SBPF_R1,  FD_SBPF_R0,  0, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_REG,   FD_SBPF_R0,  FD_SBPF_R1,  0, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
  );

  TEST_PROGRAM_SUCCESS("mod64-imm", 0x0, 4,
    FD_SBPF_INSTR(FD_SBPF_OP_LDDW,      FD_SBPF_R0,  0,      0, 0xc),
    FD_SBPF_INSTR(FD_SBPF_OP_ADDL_IMM,  0,      0,      0, 0x1),
    FD_SBPF_INSTR(FD_SBPF_OP_MOD64_IMM, FD_SBPF_R0,  0,      0, 4),
    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
  );

  TEST_PROGRAM_SUCCESS("mod64-reg", 0x0, 6,
    FD_SBPF_INSTR(FD_SBPF_OP_LDDW,      FD_SBPF_R1,  0,      0, 0xc),
    FD_SBPF_INSTR(FD_SBPF_OP_ADDL_IMM,  0,      0,      0, 0x1),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R0,  0,      0, 4),
    FD_SBPF_INSTR(FD_SBPF_OP_MOD64_REG, FD_SBPF_R1,  FD_SBPF_R0,  0, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_REG,   FD_SBPF_R0,  FD_SBPF_R1,  0, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
  );

  TEST_PROGRAM_SUCCESS("early-exit", 0x3, 4,
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R0,  0,      0, 3),
    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R0,  0,      0, 4),
    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
  );

  TEST_PROGRAM_SUCCESS("exit-not-last", 0x0, 1,
    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
  );

  TEST_PROGRAM_SUCCESS("exit", 0x0, 2,
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R0,  0,      0, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
  );

  TEST_PROGRAM_SUCCESS("ja", 0x1, 4,
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R0,  0,      0, 1),
    FD_SBPF_INSTR(FD_SBPF_OP_JA,        0,      0,     +1, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R0,  0,      0, 2),
    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
  );

  TEST_PROGRAM_SUCCESS("jeq-imm", 0x1, 8,
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R0,  0,      0, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R1,  0,      0, 0xa),
    FD_SBPF_INSTR(FD_SBPF_OP_JEQ_IMM,   FD_SBPF_R1,  0,     +4, 0xb),

    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R0,  0,      0, 1),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R1,  0,      0, 0xb),
    FD_SBPF_INSTR(FD_SBPF_OP_JEQ_IMM,   FD_SBPF_R1,  0,     +1, 0xb),

    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R0,  0,      0, 2),
    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
  );

  TEST_PROGRAM_SUCCESS("jeq-reg", 0x1, 9,
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R0,  0,      0, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R1,  0,      0, 0xa),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R2,  0,      0, 0xb),
    FD_SBPF_INSTR(FD_SBPF_OP_JEQ_REG,   FD_SBPF_R1,  FD_SBPF_R2, +4, 0),

    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R0,  0,      0, 1),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R1,  0,      0, 0xb),
    FD_SBPF_INSTR(FD_SBPF_OP_JEQ_REG,   FD_SBPF_R1,  FD_SBPF_R2, +1, 0),

    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R0,  0,      0, 2),
    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
  );

  TEST_PROGRAM_SUCCESS("jge-imm", 0x1, 8,
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R0,  0,      0, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R1,  0,      0, 4),

    FD_SBPF_INSTR(FD_SBPF_OP_JGE_IMM,   FD_SBPF_R1,  0,     +2, 6),
    FD_SBPF_INSTR(FD_SBPF_OP_JGE_IMM,   FD_SBPF_R1,  0,     +1, 5),
    FD_SBPF_INSTR(FD_SBPF_OP_JGE_IMM,   FD_SBPF_R1,  0,     +1, 4),

    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R0,  0,      0, 1),
    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
  );

  TEST_PROGRAM_SUCCESS("jge-reg", 0x1, 11,
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R0,  0,      0, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R1,  0,      0, 4),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R2,  0,      0, 6),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R3,  0,      0, 4),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R4,  0,      0, 5),

    FD_SBPF_INSTR(FD_SBPF_OP_JGE_REG,   FD_SBPF_R1,  FD_SBPF_R2, +2, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_JGE_REG,   FD_SBPF_R1,  FD_SBPF_R4, +1, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_JGE_REG,   FD_SBPF_R1,  FD_SBPF_R3, +1, 0),

    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R0,  0,      0, 1),
    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
  );

  TEST_PROGRAM_SUCCESS("jgt-imm", 0x1, 8,
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R0,  0,      0, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R1,  0,      0, 5),

    FD_SBPF_INSTR(FD_SBPF_OP_JGT_IMM,   FD_SBPF_R1,  0,     +2, 6),
    FD_SBPF_INSTR(FD_SBPF_OP_JGT_IMM,   FD_SBPF_R1,  0,     +1, 5),
    FD_SBPF_INSTR(FD_SBPF_OP_JGT_IMM,   FD_SBPF_R1,  0,     +1, 4),

    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R0,  0,      0, 1),
    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
  );

  TEST_PROGRAM_SUCCESS("jgt-reg", 0x1, 10,
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R0,  0,      0, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R1,  0,      0, 5),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R2,  0,      0, 6),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R3,  0,      0, 4),

    FD_SBPF_INSTR(FD_SBPF_OP_JGT_REG,   FD_SBPF_R1,  FD_SBPF_R2, +2, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_JGT_REG,   FD_SBPF_R1,  FD_SBPF_R1, +1, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_JGT_REG,   FD_SBPF_R1,  FD_SBPF_R3, +1, 0),

    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R0,  0,      0, 1),
    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
  );

  TEST_PROGRAM_SUCCESS("jle-imm", 0x1, 8,
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R0,  0,      0, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R1,  0,      0, 7),

    FD_SBPF_INSTR(FD_SBPF_OP_JLE_IMM,   FD_SBPF_R1,  0,     +2, 6),
    FD_SBPF_INSTR(FD_SBPF_OP_JLE_IMM,   FD_SBPF_R1,  0,     +2, 8),
    FD_SBPF_INSTR(FD_SBPF_OP_JLE_IMM,   FD_SBPF_R1,  0,     +1, 4),

    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R0,  0,      0, 1),
    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
  );

  TEST_PROGRAM_SUCCESS("jle-reg", 0x1, 11,
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R0,  0,      0, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R1,  0,      0, 10),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R2,  0,      0, 7),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R3,  0,      0, 11),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R4,  0,      0, 5),

    FD_SBPF_INSTR(FD_SBPF_OP_JLE_REG,   FD_SBPF_R1,  FD_SBPF_R2, +2, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_JLE_REG,   FD_SBPF_R1,  FD_SBPF_R4, +1, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_JLE_REG,   FD_SBPF_R1,  FD_SBPF_R3, +1, 0),

    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R0,  0,      0, 1),
    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
  );

  TEST_PROGRAM_SUCCESS("jlt-imm", 0x1, 8,
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R0,  0,      0, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R1,  0,      0, 7),

    FD_SBPF_INSTR(FD_SBPF_OP_JLT_IMM,   FD_SBPF_R1,  0,     +2, 6),
    FD_SBPF_INSTR(FD_SBPF_OP_JLT_IMM,   FD_SBPF_R1,  0,     +2, 8),
    FD_SBPF_INSTR(FD_SBPF_OP_JLT_IMM,   FD_SBPF_R1,  0,     +1, 4),

    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R0,  0,      0, 1),
    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
  );

  TEST_PROGRAM_SUCCESS("jlt-reg", 0x1, 11,
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R0,  0,      0, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R1,  0,      0, 10),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R2,  0,      0, 7),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R3,  0,      0, 11),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R4,  0,      0, 5),

    FD_SBPF_INSTR(FD_SBPF_OP_JLT_REG,   FD_SBPF_R1,  FD_SBPF_R2, +2, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_JLT_REG,   FD_SBPF_R1,  FD_SBPF_R4, +1, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_JLT_REG,   FD_SBPF_R1,  FD_SBPF_R3, +1, 0),

    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R0,  0,      0, 1),
    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
  );

  TEST_PROGRAM_SUCCESS("jne-imm", 0x1, 8,
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R0,  0,      0, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R1,  0,      0, 7),

    FD_SBPF_INSTR(FD_SBPF_OP_JNE_IMM,   FD_SBPF_R1,  0,     +2, 7),
    FD_SBPF_INSTR(FD_SBPF_OP_JNE_IMM,   FD_SBPF_R1,  0,     +2, 10),
    FD_SBPF_INSTR(FD_SBPF_OP_JNE_IMM,   FD_SBPF_R1,  0,     +1, 7),

    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R0,  0,      0, 1),
    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
  );

  TEST_PROGRAM_SUCCESS("jne-reg", 0x1, 11,
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R0,  0,      0, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R1,  0,      0, 10),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R2,  0,      0, 10),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R3,  0,      0, 24),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R4,  0,      0, 10),

    FD_SBPF_INSTR(FD_SBPF_OP_JNE_REG,   FD_SBPF_R1,  FD_SBPF_R2, +2, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_JNE_REG,   FD_SBPF_R1,  FD_SBPF_R4, +1, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_JNE_REG,   FD_SBPF_R1,  FD_SBPF_R3, +1, 0),

    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R0,  0,      0, 1),
    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
  );

  TEST_PROGRAM_SUCCESS("jset-imm", 0x1, 8,
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R0,  0,      0, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R1,  0,      0, 0x8),

    FD_SBPF_INSTR(FD_SBPF_OP_JSET_IMM,   FD_SBPF_R1,  0,     +2, 0x7),
    FD_SBPF_INSTR(FD_SBPF_OP_JSET_IMM,   FD_SBPF_R1,  0,     +2, 0x9),
    FD_SBPF_INSTR(FD_SBPF_OP_JSET_IMM,   FD_SBPF_R1,  0,     +1, 0x10),

    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R0,  0,      0, 1),
    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
  );

  TEST_PROGRAM_SUCCESS("jset-reg", 0x1, 11,
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R0,  0,      0, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R1,  0,      0, 0x8),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R2,  0,      0, 0x7),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R3,  0,      0, 0x9),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R4,  0,      0, 0x0),

    FD_SBPF_INSTR(FD_SBPF_OP_JSET_REG,   FD_SBPF_R1,  FD_SBPF_R2, +2, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_JSET_REG,   FD_SBPF_R1,  FD_SBPF_R4, +1, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_JSET_REG,   FD_SBPF_R1,  FD_SBPF_R3, +1, 0),

    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R0,  0,      0, 1),
    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
  );

  TEST_PROGRAM_SUCCESS("ldq", 0x1122334455667788, 3,
    FD_SBPF_INSTR(FD_SBPF_OP_LDDW,      FD_SBPF_R0,  0,      0, 0x55667788),
    FD_SBPF_INSTR(FD_SBPF_OP_ADDL_IMM,  0,      0,      0, 0x11223344),
    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
  );

  TEST_PROGRAM_SUCCESS("stb-heap", 0x11, 5,
    FD_SBPF_INSTR(FD_SBPF_OP_LDDW,      FD_SBPF_R1,  0,      0, 0x0),
    FD_SBPF_INSTR(FD_SBPF_OP_ADDL_IMM,  0,      0,      0, 0x3),
    FD_SBPF_INSTR(FD_SBPF_OP_STB,       FD_SBPF_R1,  0,     +2, 0x11),
    FD_SBPF_INSTR(FD_SBPF_OP_LDXB,      FD_SBPF_R0,  FD_SBPF_R1, +2, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
  );

  TEST_PROGRAM_SUCCESS("sth-heap", 0x1122, 5,
    FD_SBPF_INSTR(FD_SBPF_OP_LDDW,      FD_SBPF_R1,  0,      0, 0x0),
    FD_SBPF_INSTR(FD_SBPF_OP_ADDL_IMM,  0,      0,      0, 0x3),
    FD_SBPF_INSTR(FD_SBPF_OP_STH,       FD_SBPF_R1,  0,     +2, 0x1122),
    FD_SBPF_INSTR(FD_SBPF_OP_LDXH,      FD_SBPF_R0,  FD_SBPF_R1, +2, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
  );

  TEST_PROGRAM_SUCCESS("stw-heap", 0x11223344, 5,
    FD_SBPF_INSTR(FD_SBPF_OP_LDDW,      FD_SBPF_R1,  0,      0, 0x0),
    FD_SBPF_INSTR(FD_SBPF_OP_ADDL_IMM,  0,      0,      0, 0x3),
    FD_SBPF_INSTR(FD_SBPF_OP_STW,       FD_SBPF_R1,  0,     +2, 0x11223344),
    FD_SBPF_INSTR(FD_SBPF_OP_LDXW,      FD_SBPF_R0,  FD_SBPF_R1, +2, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
  );

  // TODO: check that we zero upper 32 bits
  TEST_PROGRAM_SUCCESS("stq-heap", 0x11223344, 5,
    FD_SBPF_INSTR(FD_SBPF_OP_LDDW,      FD_SBPF_R1,  0,      0, 0x0),
    FD_SBPF_INSTR(FD_SBPF_OP_ADDL_IMM,  0,      0,      0, 0x3),
    FD_SBPF_INSTR(FD_SBPF_OP_STDW,      FD_SBPF_R1,  0,     +2, 0x11223344),
    FD_SBPF_INSTR(FD_SBPF_OP_LDXDW,     FD_SBPF_R0,  FD_SBPF_R1, +2, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
  );

  TEST_PROGRAM_SUCCESS("stxb-heap", 0x11, 6,
    FD_SBPF_INSTR(FD_SBPF_OP_LDDW,      FD_SBPF_R1,  0,      0, 0x0),
    FD_SBPF_INSTR(FD_SBPF_OP_ADDL_IMM,  0,      0,      0, 0x3),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R2,  0,      0, 0x11),
    FD_SBPF_INSTR(FD_SBPF_OP_STXB,      FD_SBPF_R1,  FD_SBPF_R2, +2, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_LDXB,      FD_SBPF_R0,  FD_SBPF_R1, +2, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
  );

  TEST_PROGRAM_SUCCESS("stxh-heap", 0x1122, 6,
    FD_SBPF_INSTR(FD_SBPF_OP_LDDW,      FD_SBPF_R1,  0,      0, 0x0),
    FD_SBPF_INSTR(FD_SBPF_OP_ADDL_IMM,  0,      0,      0, 0x3),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R2,  0,      0, 0x1122),
    FD_SBPF_INSTR(FD_SBPF_OP_STXH,      FD_SBPF_R1,  FD_SBPF_R2, +2, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_LDXH,      FD_SBPF_R0,  FD_SBPF_R1, +2, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
  );

  TEST_PROGRAM_SUCCESS("stxw-heap", 0x11223344, 6,
    FD_SBPF_INSTR(FD_SBPF_OP_LDDW,      FD_SBPF_R1,  0,      0, 0x0),
    FD_SBPF_INSTR(FD_SBPF_OP_ADDL_IMM,  0,      0,      0, 0x3),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV_IMM,   FD_SBPF_R2,  0,      0, 0x11223344),
    FD_SBPF_INSTR(FD_SBPF_OP_STXW,      FD_SBPF_R1,  FD_SBPF_R2, +2, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_LDXW,      FD_SBPF_R0,  FD_SBPF_R1, +2, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
  );

  TEST_PROGRAM_SUCCESS("stxq-heap", 0x1122334455667788, 7,
    FD_SBPF_INSTR(FD_SBPF_OP_LDDW,      FD_SBPF_R1,  0,      0, 0x0),
    FD_SBPF_INSTR(FD_SBPF_OP_ADDL_IMM,  0,      0,      0, 0x3),
    FD_SBPF_INSTR(FD_SBPF_OP_LDDW,      FD_SBPF_R2,  0,      0, 0x55667788),
    FD_SBPF_INSTR(FD_SBPF_OP_ADDL_IMM,  0,      0,      0, 0x11223344),
    FD_SBPF_INSTR(FD_SBPF_OP_STXDW,     FD_SBPF_R1,  FD_SBPF_R2, +2, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_LDXDW,     FD_SBPF_R0,  FD_SBPF_R1, +2, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
  );

  TEST_PROGRAM_SUCCESS("prime", 0x1, 16,
    FD_SBPF_INSTR(FD_SBPF_OP_MOV64_IMM, FD_SBPF_R1,  0,      0, 10007),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV64_IMM, FD_SBPF_R0,  0,      0, 0x1),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV64_IMM, FD_SBPF_R2,  0,      0, 0x2),
    FD_SBPF_INSTR(FD_SBPF_OP_JGT_IMM,   FD_SBPF_R1,  0,     +4, 0x2),

    FD_SBPF_INSTR(FD_SBPF_OP_JA,        0,      0,    +10, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_ADD64_IMM, FD_SBPF_R2,  0,      0, 0x1),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV64_IMM, FD_SBPF_R0,  0,      0, 0x1),
    FD_SBPF_INSTR(FD_SBPF_OP_JGE_REG,   FD_SBPF_R2,  FD_SBPF_R1, +7, 0),

    FD_SBPF_INSTR(FD_SBPF_OP_MOV64_REG, FD_SBPF_R3,  FD_SBPF_R1,  0, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_DIV64_REG, FD_SBPF_R3,  FD_SBPF_R2,  0, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_MUL64_REG, FD_SBPF_R3,  FD_SBPF_R2,  0, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV64_REG, FD_SBPF_R4,  FD_SBPF_R1,  0, 0),

    FD_SBPF_INSTR(FD_SBPF_OP_SUB64_REG, FD_SBPF_R4,  FD_SBPF_R3,  0, 0),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV64_IMM, FD_SBPF_R0,  0,      0, 0x0),
    FD_SBPF_INSTR(FD_SBPF_OP_JNE_IMM,   FD_SBPF_R4,  0,    -10, 0x0),
    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
  );

  TEST_PROGRAM_SUCCESS("syscall", 15, 7,
    FD_SBPF_INSTR(FD_SBPF_OP_MOV64_IMM, FD_SBPF_R1,  0,      0, 1),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV64_IMM, FD_SBPF_R2,  0,      0, 2),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV64_IMM, FD_SBPF_R3,  0,      0, 3),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV64_IMM, FD_SBPF_R4,  0,      0, 4),
    FD_SBPF_INSTR(FD_SBPF_OP_MOV64_IMM, FD_SBPF_R5,  0,      0, 5),
    FD_SBPF_INSTR(FD_SBPF_OP_CALL_IMM,      0,      0,      0, fd_murmur3_32( "accumulator", 11UL, 0U ) ),

    FD_SBPF_INSTR(FD_SBPF_OP_EXIT,      0,      0,      0, 0),
  );

  ulong   text_cnt = 128*1024*1024;
  ulong * text     = (ulong *)malloc( sizeof(ulong)*text_cnt ); /* FIXME: gross */

  generate_random_alu_instrs( rng, text, text_cnt );
  test_program_success( "alu_bench", 0x0, text, text_cnt, syscalls, instr_ctx );

  generate_random_alu64_instrs( rng, text, text_cnt );
  test_program_success( "alu64_bench", 0x0, text, text_cnt, syscalls, instr_ctx );

  text_cnt = 1024UL;
  generate_random_alu_instrs( rng, text, text_cnt );
  test_program_success( "alu_bench_short", 0x0, text, text_cnt, syscalls, instr_ctx );

  generate_random_alu64_instrs( rng, text, text_cnt );
  test_program_success( "alu64_bench_short", 0x0, text, text_cnt, syscalls, instr_ctx );

  test_0cu_exit();

  free( text );

  fd_sbpf_syscalls_delete( fd_sbpf_syscalls_leave( syscalls ) );
  fd_valloc_free( valloc, slot_ctx );
  test_vm_exec_instr_ctx_delete( instr_ctx, valloc );

  test_static_syscalls_list();

  FD_LOG_NOTICE(( "pass" ));
  fd_rng_delete( fd_rng_leave( rng ) );
  fd_halt();
  return 0;
}
