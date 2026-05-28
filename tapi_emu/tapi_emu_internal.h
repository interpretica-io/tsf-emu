/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief Emulator TAPI: internal helpers
 *
 * Internal to tsf-emu; not installed.
 */

#ifndef __TSF_TAPI_EMU_INTERNAL_H__
#define __TSF_TAPI_EMU_INTERNAL_H__

#include "te_defs.h"
#include "te_errno.h"
#include "te_string.h"
#include "te_vector.h"
#include "tapi_job.h"

#include "tapi_devtool_run.h"

#include "tapi_emu.h"

#ifdef __cplusplus
extern "C" {
#endif

/** A machine, as this library holds it. */
struct tapi_emu_vm {
    /** The QEMU job. */
    tapi_devtool_run run;
    /** Job factory it was started through. */
    tapi_job_factory_t *factory;
    /** Agent it runs on. */
    char *ta;
    /** Path of the QMP socket on the agent. */
    char *qmp_socket;
    /** Path of the QMP helper script on the agent. */
    char *helper;
    /** The python interpreter to run the helper with. */
    char *python;
    /** @c true once it has been taken down. */
    bool gone;
};

/** Append one argument to a vector, taking ownership of it. */
extern void tapi_emu_arg(te_vec *args, const char *fmt, ...)
    TE_LIKE_PRINTF(2, 3);

/**
 * Run a tool on the agent and wait for it.
 *
 * @param[in]  factory      Job factory.
 * @param[in]  name         Tool name for log messages.
 * @param[in]  program      Program name or path.
 * @param[in]  args         Arguments after @c argv[0].
 * @param[in]  timeout_ms   Timeout, ms.
 * @param[out] out          Standard output, or @c NULL.
 * @param[out] err          Standard error, or @c NULL.
 * @param[out] exit_code    Exit status, or @c NULL.
 *
 * @return Status code of running the tool, not of the tool.
 */
extern te_errno tapi_emu_cmd(tapi_job_factory_t *factory, const char *name,
                             const char *program, const te_vec *args,
                             int timeout_ms, te_string *out, te_string *err,
                             int *exit_code);

/**
 * Start a tool and leave it running.
 *
 * @param[in]  factory      Job factory.
 * @param[in]  name         Tool name for log messages.
 * @param[in]  program      Program name or path.
 * @param[in]  args         Arguments after @c argv[0].
 * @param[out] run          Run handle.
 *
 * @return Status code.
 */
extern te_errno tapi_emu_spawn(tapi_job_factory_t *factory,
                               const char *name, const char *program,
                               const te_vec *args, tapi_devtool_run *run);

/** Put the QMP helper script on the agent. */
extern te_errno tapi_emu_put_helper(tapi_emu_vm *vm);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* !__TSF_TAPI_EMU_INTERNAL_H__ */
