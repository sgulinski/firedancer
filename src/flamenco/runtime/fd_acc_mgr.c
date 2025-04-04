#include "fd_acc_mgr.h"
#include "fd_runtime.h"
#include "../../ballet/base58/fd_base58.h"
#include "context/fd_exec_epoch_ctx.h"
#include "context/fd_exec_slot_ctx.h"
#include "fd_rent_lists.h"
#include "fd_rocksdb.h"
#include "sysvar/fd_sysvar_rent.h"
#include "fd_system_ids.h"
#include <assert.h>

fd_acc_mgr_t *
fd_acc_mgr_new( void *      mem,
                fd_funk_t * funk ) {

  if( FD_UNLIKELY( !mem ) ) {
    FD_LOG_WARNING(( "NULL mem" ));
    return NULL;
  }

  if( FD_UNLIKELY( !fd_ulong_is_aligned( (ulong)mem, FD_ACC_MGR_ALIGN ) ) ) {
    FD_LOG_WARNING(( "misaligned mem" ));
    return NULL;
  }

  fd_memset( mem, 0, FD_ACC_MGR_FOOTPRINT );

  fd_acc_mgr_t * acc_mgr = fd_type_pun( mem );
  acc_mgr->funk = funk;
  return acc_mgr;

}

void *
fd_acc_mgr_delete( fd_acc_mgr_t * acc_mgr ) {

  if( FD_UNLIKELY( !acc_mgr ) ) return NULL;

  memset( acc_mgr, 0, FD_ACC_MGR_FOOTPRINT );
  return acc_mgr;
}

void
fd_acc_mgr_set_slots_per_epoch( fd_exec_slot_ctx_t * slot_ctx,
                                ulong                slots_per_epoch ) {
  fd_acc_mgr_t * acc_mgr   = slot_ctx->acc_mgr;
  acc_mgr->slots_per_epoch = slots_per_epoch;
  acc_mgr->part_width      = fd_rent_partition_width( slots_per_epoch );
}

fd_account_meta_t const *
fd_acc_mgr_view_raw( fd_acc_mgr_t *         acc_mgr,
                     fd_funk_txn_t const *  txn,
                     fd_pubkey_t const *    pubkey,
                     fd_funk_rec_t const ** orec,
                     int *                  opt_err,
                     fd_funk_txn_t const ** txn_out  ) {

  fd_funk_rec_key_t id   = fd_acc_funk_key( pubkey );
  fd_funk_t *       funk = acc_mgr->funk;

  fd_funk_rec_t const * rec = fd_funk_rec_query_global( funk, txn, &id, txn_out );

  if( FD_UNLIKELY( !rec || !!( rec->flags & FD_FUNK_REC_FLAG_ERASE ) ) )  {
    fd_int_store_if( !!opt_err, opt_err, FD_ACC_MGR_ERR_UNKNOWN_ACCOUNT );
    return NULL;
  }
  if( NULL != orec )
    *orec = rec;

  void const * raw = fd_funk_val( rec, fd_funk_wksp(funk) );
  // TODO/FIXME: this check causes issues with some metadata writes

  fd_account_meta_t const * metadata = fd_type_pun_const( raw );
  if( FD_UNLIKELY( metadata->magic != FD_ACCOUNT_META_MAGIC ) ) {
    fd_int_store_if( !!opt_err, opt_err, FD_ACC_MGR_ERR_WRONG_MAGIC );
    return NULL;
  }

  return metadata;
}

int
fd_acc_mgr_view( fd_acc_mgr_t *        acc_mgr,
                 fd_funk_txn_t const * txn,
                 fd_pubkey_t const *   pubkey,
                 fd_txn_account_t *    account) {
  /* TODO: re-add this check after consulting on why this builtin program check.
     Is it the case that the  */
  // if( fd_pubkey_is_builtin_program( pubkey )
  //     || memcmp(pubkey->uc, fd_solana_compute_budget_program_id.uc, sizeof(fd_pubkey_t))==0 ) {
  //   txn = NULL;
  // }
  int err = FD_ACC_MGR_SUCCESS;
  fd_account_meta_t const * meta = fd_acc_mgr_view_raw( acc_mgr, txn, pubkey, &account->const_rec, &err, NULL );
  if( FD_UNLIKELY( !fd_acc_exists( meta ) ) ) {
    if( FD_UNLIKELY( err!=FD_ACC_MGR_SUCCESS ) ) {
      return err;
    }
    return FD_ACC_MGR_ERR_UNKNOWN_ACCOUNT;
  }

  if( FD_UNLIKELY( FD_TXN_ACCOUNT_MAGIC != account->magic ) ) {
    FD_LOG_ERR(( "bad magic for borrowed account - acc: %s, expected: %016lx, got: %016lx", FD_BASE58_ENC_32_ALLOCA( pubkey->uc ), FD_TXN_ACCOUNT_MAGIC, account->magic ));
  }

  fd_memcpy(account->pubkey, pubkey, sizeof(fd_pubkey_t));

  account->orig_rec  = account->const_rec;
  account->orig_meta = account->const_meta = meta;
  account->orig_data = account->const_data = (uchar const *)meta + meta->hlen;

  if( ULONG_MAX == account->starting_dlen )
    account->starting_dlen = meta->dlen;

  if( ULONG_MAX == account->starting_lamports )
    account->starting_lamports = meta->info.lamports;

  return FD_ACC_MGR_SUCCESS;
}

fd_account_meta_t *
fd_acc_mgr_modify_raw( fd_acc_mgr_t *        acc_mgr,
                       fd_funk_txn_t *       txn,
                       fd_pubkey_t const *   pubkey,
                       int                   do_create,
                       ulong                 min_data_sz,
                       fd_funk_rec_t const * opt_con_rec,
                       fd_funk_rec_t **      opt_out_rec,
                       int *                 opt_err ) {

  fd_funk_t *       funk = acc_mgr->funk;

  fd_funk_rec_key_t id   = fd_acc_funk_key( pubkey );

//#ifdef VLOG
//  ulong rec_cnt = 0;
//  for( fd_funk_rec_t const * rec = fd_funk_txn_first_rec( funk, txn );
//       NULL != rec;
//       rec = fd_funk_txn_next_rec( funk, rec ) ) {
//
//    if( !fd_funk_key_is_acc( rec->pair.key  ) ) continue;
//
//    FD_LOG_DEBUG(( "fd_acc_mgr_modify_raw: %s create: %s  rec_cnt: %d", FD_BASE58_ENC_32_ALLOCA( rec->pair.key->uc ), do_create ? "true" : "false", rec_cnt));
//
//    rec_cnt++;
//  }
//
//  FD_LOG_DEBUG(( "fd_acc_mgr_modify_raw: %s create: %s", FD_BASE58_ENC_32_ALLOCA( pubkey->uc ), do_create ? "true" : "false"));
//#endif

  int funk_err = FD_FUNK_SUCCESS;
  fd_funk_rec_t * rec = fd_funk_rec_write_prepare( funk, txn, &id, sizeof(fd_account_meta_t)+min_data_sz, do_create, opt_con_rec, &funk_err );

  if( FD_UNLIKELY( !rec ) )  {
    if( FD_LIKELY( funk_err==FD_FUNK_ERR_KEY ) ) {
      fd_int_store_if( !!opt_err, opt_err, FD_ACC_MGR_ERR_UNKNOWN_ACCOUNT );
      return NULL;
    }
    /* Irrecoverable funky internal error [[noreturn]] */
    FD_LOG_ERR(( "fd_funk_rec_write_prepare(%s) failed (%i-%s)", FD_BASE58_ENC_32_ALLOCA( pubkey->key ), funk_err, fd_funk_strerror( funk_err ) ));
  }

  if (NULL != opt_out_rec)
    *opt_out_rec = rec;

  fd_account_meta_t * ret = fd_funk_val( rec, fd_funk_wksp( funk ) );

  if( do_create && ret->magic==0UL ) {
    fd_account_meta_init( ret );
  }

  if( ret->magic != FD_ACCOUNT_META_MAGIC ) {
    fd_int_store_if( !!opt_err, opt_err, FD_ACC_MGR_ERR_WRONG_MAGIC );
    return NULL;
  }

  return ret;
}

int
fd_acc_mgr_modify( fd_acc_mgr_t *      acc_mgr,
                   fd_funk_txn_t *     txn,
                   fd_pubkey_t const * pubkey,
                   int                 do_create,
                   ulong               min_data_sz,
                   fd_txn_account_t *  account ) {
  int err = FD_ACC_MGR_SUCCESS;

  fd_account_meta_t * meta = fd_acc_mgr_modify_raw( acc_mgr, txn, pubkey, do_create, min_data_sz, account->const_rec, &account->rec, &err );
  if( FD_UNLIKELY( !meta ) ) return err;

  assert( account->magic == FD_TXN_ACCOUNT_MAGIC );

  fd_memcpy(account->pubkey, pubkey, sizeof(fd_pubkey_t));

  if( FD_UNLIKELY( meta->magic != FD_ACCOUNT_META_MAGIC ) )
    return FD_ACC_MGR_ERR_WRONG_MAGIC;

#ifdef VLOG
  FD_LOG_DEBUG(( "fd_acc_mgr_modify: %s create: %s  lamports: %ld  owner: %s  executable: %s,  rent_epoch: %ld, data_len: %ld",
                 FD_BASE58_ENC_32_ALLOCA( pubkey->uc ),
                 do_create ? "true" : "false",
                 meta->info.lamports,
                 FD_BASE58_ENC_32_ALLOCA( meta->info.owner ),
                 meta->info.executable ? "true" : "false",
                 meta->info.rent_epoch, meta->dlen ));
#endif

  account->orig_rec  = account->const_rec  = account->rec;
  account->orig_meta = account->const_meta = account->meta = meta;
  account->orig_data = account->const_data = account->data = (uchar *)meta + meta->hlen;

  if( ULONG_MAX == account->starting_dlen )
    account->starting_dlen = meta->dlen;

  if( ULONG_MAX == account->starting_lamports )
    account->starting_lamports = meta->info.lamports;

  return FD_ACC_MGR_SUCCESS;
}

FD_FN_CONST char const *
fd_acc_mgr_strerror( int err ) {
  switch( err ) {
  case FD_ACC_MGR_SUCCESS:
    return "success";
  case FD_ACC_MGR_ERR_UNKNOWN_ACCOUNT:
    return "unknown account";
  case FD_ACC_MGR_ERR_WRITE_FAILED:
    return "write failed";
  case FD_ACC_MGR_ERR_READ_FAILED:
    return "read failed";
  case FD_ACC_MGR_ERR_WRONG_MAGIC:
    return "wrong magic";
  default:
    return "unknown";
  }
}

int
fd_acc_mgr_save( fd_acc_mgr_t *     acc_mgr,
                 fd_txn_account_t * account ) {
  if( account->meta == NULL || account->rec == NULL ) {
    // The meta is NULL so the account is not writable.
    FD_LOG_DEBUG(( "fd_acc_mgr_save: account is not writable: %s", FD_BASE58_ENC_32_ALLOCA( account->pubkey ) ));
    return FD_ACC_MGR_SUCCESS;
  }

  fd_wksp_t * wksp = fd_funk_wksp( acc_mgr->funk );
  ulong reclen = sizeof(fd_account_meta_t)+account->const_meta->dlen;
  uchar * raw = fd_funk_val( account->rec, wksp );
  fd_memcpy( raw, account->meta, reclen );

  return FD_ACC_MGR_SUCCESS;
}

int
fd_acc_mgr_save_non_tpool( fd_acc_mgr_t *     acc_mgr,
                           fd_funk_txn_t *    txn,
                           fd_txn_account_t * account ) {

  fd_funk_start_write( acc_mgr->funk );
  fd_funk_rec_key_t key = fd_acc_funk_key( account->pubkey );
  fd_funk_t * funk = acc_mgr->funk;
  fd_funk_rec_t * rec = (fd_funk_rec_t *)fd_funk_rec_query( funk, txn, &key );
  if( rec == NULL ) {
    int err;
    rec = (fd_funk_rec_t *)fd_funk_rec_insert( funk, txn, &key, &err );
    if( rec == NULL ) FD_LOG_ERR(( "unable to insert a new record, error %d", err ));
  }
  account->rec = rec;
  ulong reclen = sizeof(fd_account_meta_t)+account->const_meta->dlen;
  fd_wksp_t * wksp = fd_funk_wksp( acc_mgr->funk );
  int err;
  if( fd_funk_val_truncate( account->rec, reclen, fd_funk_alloc( acc_mgr->funk, wksp ), wksp, &err ) == NULL ) {
    FD_LOG_ERR(( "unable to allocate account value, err %d", err ));
  }
  err = fd_acc_mgr_save( acc_mgr, account );
  fd_funk_end_write( acc_mgr->funk );
  return err;
}

void
fd_acc_mgr_lock( fd_acc_mgr_t * acc_mgr ) {
  FD_TEST( !acc_mgr->is_locked );
  acc_mgr->is_locked = 1;
}

void
fd_acc_mgr_unlock( fd_acc_mgr_t * acc_mgr ) {
  FD_TEST( acc_mgr->is_locked );
  acc_mgr->is_locked = 0;
}

struct fd_acc_mgr_save_task_args {
  fd_acc_mgr_t * acc_mgr;
};
typedef struct fd_acc_mgr_save_task_args fd_acc_mgr_save_task_args_t;

struct fd_acc_mgr_save_task_info {
  fd_txn_account_t * * accounts;
  ulong accounts_cnt;
  int result;
};
typedef struct fd_acc_mgr_save_task_info fd_acc_mgr_save_task_info_t;

static void
fd_acc_mgr_save_task( void *tpool,
                      ulong t0 FD_PARAM_UNUSED, ulong t1 FD_PARAM_UNUSED,
                      void *args,
                      void *reduce FD_PARAM_UNUSED, ulong stride FD_PARAM_UNUSED,
                      ulong l0 FD_PARAM_UNUSED, ulong l1 FD_PARAM_UNUSED,
                      ulong m0, ulong m1 FD_PARAM_UNUSED,
                      ulong n0 FD_PARAM_UNUSED, ulong n1 FD_PARAM_UNUSED ) {
  fd_acc_mgr_save_task_args_t * task_args = (fd_acc_mgr_save_task_args_t *)args;
  fd_acc_mgr_save_task_info_t * task_info = (fd_acc_mgr_save_task_info_t *)tpool + m0;

  for( ulong i = 0; i < task_info->accounts_cnt; i++ ) {
    int err = fd_acc_mgr_save(task_args->acc_mgr,task_info->accounts[i] );
    if( FD_UNLIKELY( err != FD_ACC_MGR_SUCCESS ) ) {
      task_info->result = err;
      return;
    }
  }
  task_info->result = FD_ACC_MGR_SUCCESS;
}

int
fd_acc_mgr_save_many_tpool( fd_acc_mgr_t *       acc_mgr,
                            fd_funk_txn_t *      txn,
                            fd_txn_account_t * * accounts,
                            ulong                accounts_cnt,
                            fd_tpool_t *         tpool,
                            fd_spad_t *          runtime_spad ) {

  FD_SPAD_FRAME_BEGIN( runtime_spad ) {

  fd_funk_t *     funk    = acc_mgr->funk;
  fd_wksp_t *     wksp    = fd_funk_wksp( funk );
  fd_funk_rec_t * rec_map = fd_funk_rec_map( funk, wksp );

  ulong batch_cnt = fd_ulong_min(
    fd_funk_rec_map_private_list_cnt( fd_funk_rec_map_key_max( rec_map ) ),
    fd_ulong_pow2_up( fd_tpool_worker_cnt( tpool ) )
  );
  ulong batch_mask = (batch_cnt - 1UL);

  ulong * batch_szs = fd_spad_alloc( runtime_spad, 8UL, batch_cnt * sizeof(ulong) );
  fd_memset( batch_szs, 0, batch_cnt * sizeof(ulong) );

  /* Compute the batch sizes */
  for( ulong i = 0; i < accounts_cnt; i++ ) {
    ulong batch_idx = i & batch_mask;
    batch_szs[batch_idx]++;
  }

  fd_txn_account_t * *          task_accounts        = fd_spad_alloc( runtime_spad, 8UL, accounts_cnt * sizeof(fd_txn_account_t *) );
  fd_acc_mgr_save_task_info_t * task_infos           = fd_spad_alloc( runtime_spad, 8UL, batch_cnt * sizeof(fd_acc_mgr_save_task_info_t) );
  fd_txn_account_t * *          task_accounts_cursor = task_accounts;

  /* Construct the batches */
  for( ulong i = 0; i < batch_cnt; i++ ) {
    ulong batch_sz = batch_szs[i];
    fd_acc_mgr_save_task_info_t * task_info = &task_infos[i];

    task_info->accounts_cnt = 0;
    task_info->accounts = task_accounts_cursor;
    task_info->result = 0;

    task_accounts_cursor += batch_sz;
  }

  fd_funk_start_write( funk );

  for( ulong i = 0; i < accounts_cnt; i++ ) {
    fd_txn_account_t * account = accounts[i];

    ulong batch_idx = i & batch_mask;
    fd_acc_mgr_save_task_info_t * task_info = &task_infos[batch_idx];
    task_info->accounts[task_info->accounts_cnt++] = account;
    fd_funk_rec_key_t key = fd_acc_funk_key( account->pubkey );
    fd_funk_rec_t * rec = (fd_funk_rec_t *)fd_funk_rec_query( funk, txn, &key );
    if( rec == NULL ) {
      int err;
      rec = (fd_funk_rec_t *)fd_funk_rec_insert( funk, txn, &key, &err );
      if( rec == NULL ) FD_LOG_ERR(( "unable to insert a new record, error %d", err ));
    }
    account->rec = rec;

    /* This check is to prevent a seg fault in the case where an account with
        null data tries to get saved. This notably happens if firedancer is
        attemping to execute a bad block. This should NEVER happen in the case
        of a proper replay. */
    if( FD_UNLIKELY( !account->const_meta ) ) {
      FD_LOG_ERR(( "An account likely does not exist. This block could be invalid." ));
    }

    ulong reclen = sizeof(fd_account_meta_t)+account->const_meta->dlen;
    int err;
    if( FD_UNLIKELY( NULL == fd_funk_val_truncate( account->rec,
                                                   reclen,
                                                   fd_funk_alloc( acc_mgr->funk, wksp ),
                                                   wksp,
                                                   &err ) ) ) {
      FD_LOG_ERR(( "unable to allocate account value, err %d", err ));
    }
  }

  fd_acc_mgr_save_task_args_t task_args = {
    .acc_mgr = acc_mgr
  };

  /* Save accounts in a thread pool */

  fd_tpool_exec_all_rrobin( tpool, 0, fd_tpool_worker_cnt( tpool ), fd_acc_mgr_save_task,
                            task_infos, &task_args, NULL, 1, 0, batch_cnt );

  fd_funk_end_write( funk );

  /* Check results */
  for( ulong i = 0; i < batch_cnt; i++ ) {
    fd_acc_mgr_save_task_info_t * task_info = &task_infos[i];
    if( task_info->result != FD_ACC_MGR_SUCCESS ) {
      return task_info->result;
    }
  }

  return FD_ACC_MGR_SUCCESS;

  } FD_SPAD_FRAME_END;
}
