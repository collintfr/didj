# Applied to the fetched Unicorn source (FetchContent PATCH_COMMAND).
#
# Unicorn emits an exit check after guest memory accesses. An asynchronous
# uc_emu_stop() (our time slicing) trips it in the middle of a translation
# block, and QEMU then resumes at the start of the block, re-executing the
# instructions before the access. That silently corrupts guest state, so
# exits are limited to block boundaries. Idempotent.
set(file "${UNICORN_SOURCE_DIR}/qemu/tcg/tcg-op.c")
file(READ "${file}" src)
string(FIND "${src}" "didj: exits are only taken" already)
if(already GREATER -1)
  return()
endif()
set(anchor "void check_exit_request(TCGContext *tcg_ctx)\n{\n    TCGv_i32 count;\n")
string(FIND "${src}" "${anchor}" pos)
if(pos EQUAL -1)
  message(FATAL_ERROR "patch_unicorn: check_exit_request() not found; Unicorn version changed?")
endif()
string(REPLACE "${anchor}" "${anchor}
    // didj: exits are only taken at translation-block boundaries; see
    // cmake/patch_unicorn.cmake in the didj repository.
    if (1) {
        return;
    }
" src "${src}")
file(WRITE "${file}" "${src}")
