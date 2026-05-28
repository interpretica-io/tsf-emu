/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief Emulator TAPI: running a tool on an agent
 */

#define TE_LGR_USER "TAPI EMU"

#include "te_config.h"

#include <stdarg.h>
#include <stdlib.h>

#include "logger_api.h"
#include "te_string.h"
#include "te_vector.h"

#include "tapi_emu_internal.h"

/** Arguments of a command, as a plain vector of strings. */
typedef struct emu_cmd_opt {
    /** Number of arguments. */
    size_t n_args;
    /** Arguments after argv[0]. */
    const char **args;
} emu_cmd_opt;

static const tapi_job_opt_bind emu_cmd_binds[] = TAPI_JOB_OPT_SET(
    TAPI_JOB_OPT_ARRAY_PTR(emu_cmd_opt, n_args, args,
        TAPI_JOB_OPT_CONTENT(TAPI_JOB_OPT_STRING, NULL, false))
);

/* See description in tapi_emu_internal.h */
void
tapi_emu_arg(te_vec *args, const char *fmt, ...)
{
    te_string built = TE_STRING_INIT;
    char *arg;
    va_list ap;

    va_start(ap, fmt);
    te_string_append_va(&built, fmt, ap);
    va_end(ap);

    arg = built.ptr;
    TE_VEC_APPEND(args, arg);
}

/* See description in tapi_emu_internal.h */
te_errno
tapi_emu_cmd(tapi_job_factory_t *factory, const char *name,
             const char *program, const te_vec *args, int timeout_ms,
             te_string *out, te_string *err, int *exit_code)
{
    emu_cmd_opt opt = {
        .n_args = te_vec_size(args),
        .args = te_vec_size(args) == 0 ? NULL :
                (const char **)te_vec_get((te_vec *)args, 0),
    };
    tapi_devtool_output output;
    tapi_devtool_run run = TAPI_DEVTOOL_RUN_INIT;
    te_errno rc;

    rc = tapi_devtool_run_init(&run, factory, name, program, emu_cmd_binds,
                               &opt, NULL);
    if (rc != 0)
        return rc;

    rc = tapi_devtool_run_start(&run);
    if (rc == 0)
        rc = tapi_devtool_run_wait(&run, timeout_ms);

    if (rc != 0)
    {
        tapi_devtool_run_fini(&run);
        return rc;
    }

    tapi_devtool_run_get_output(&run, &output);

    if (out != NULL && output.out != NULL)
        te_string_append(out, "%s", output.out);
    if (err != NULL && output.err != NULL)
        te_string_append(err, "%s", output.err);

    if (exit_code != NULL)
    {
        *exit_code = output.status.type == TAPI_JOB_STATUS_EXITED ?
                     output.status.value : -1;
    }

    return tapi_devtool_run_fini(&run);
}

/* See description in tapi_emu_internal.h */
te_errno
tapi_emu_spawn(tapi_job_factory_t *factory, const char *name,
               const char *program, const te_vec *args,
               tapi_devtool_run *run)
{
    emu_cmd_opt opt = {
        .n_args = te_vec_size(args),
        .args = te_vec_size(args) == 0 ? NULL :
                (const char **)te_vec_get((te_vec *)args, 0),
    };
    te_errno rc;

    *run = (tapi_devtool_run)TAPI_DEVTOOL_RUN_INIT;

    rc = tapi_devtool_run_init(run, factory, name, program, emu_cmd_binds,
                               &opt, NULL);
    if (rc != 0)
        return rc;

    /* Started and left running: a machine outlives the call that made it. */
    rc = tapi_devtool_run_start(run);
    if (rc != 0)
        tapi_devtool_run_fini(run);

    return rc;
}
