ifdef FD_HAS_HOSTED
ifdef FD_HAS_INT128
ifdef FD_HAS_ZSTD
$(call add-hdrs,fd_store.h fd_pending_slots.h fd_trusted_slots.h fd_epoch_forks.h)
$(call add-objs,fd_store fd_pending_slots fd_trusted_slots fd_epoch_forks,fd_discof)
$(call make-unit-test,test_trusted_slots,test_trusted_slots,fd_discof fd_util)
$(call make-unit-test,test_epoch_forks,test_epoch_forks,fd_discof fd_flamenco fd_ballet fd_util)
ifdef FD_HAS_SSE
$(call add-objs,fd_storei_tile,fd_discof)
endif
endif
endif
endif
