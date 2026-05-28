/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief The protocol QEMU listens to
 *
 * @defgroup tapi_emu_qmp QMP (tapi_emu_qmp)
 * @ingroup tapi_emu
 * @{
 *
 * QMP is how a running QEMU is driven. It is a JSON protocol on a
 * socket, and everything @ref tapi_emu does after the machine has
 * started goes through it. This is the way in for the rest - device
 * hotplug, migration, block jobs, tracing - which this library does
 * not wrap and does not need to.
 *
 * @code
 * te_string reply = TE_STRING_INIT;
 *
 * CHECK_RC(tapi_emu_qmp(vm, "query-block", NULL, 10000, &reply));
 * RING("%s", reply.ptr);
 * @endcode
 *
 * @section tapi_emu_qmp_how How it reaches the agent
 *
 * The socket is on the agent, so something on the agent has to speak
 * to it. That something is a small @c python3 script this library
 * writes there once and runs per command: it connects, performs the
 * handshake, sends one command, prints the reply and exits.
 *
 * One process per command, deliberately. A persistent channel would
 * be faster and would have to be kept alive across a test that fails
 * half way; QMP commands are not in hot loops, and a stateless helper
 * cannot leave anything behind.
 *
 * @section tapi_emu_qmp_events Events are not replies
 *
 * QEMU sends events down the same socket, whenever it likes, in
 * between everything else. Measured on QEMU 10.2.0: a @c quit command
 * gets a @c SHUTDOWN event *before* its own reply. So a client that
 * reads one line and calls it an answer will sooner or later read an
 * event instead and report nonsense. The helper skips anything with an
 * @c event key and keeps reading.
 */

#ifndef __TSF_TAPI_EMU_QMP_H__
#define __TSF_TAPI_EMU_QMP_H__

#include "te_defs.h"
#include "te_errno.h"
#include "te_string.h"

#include "tapi_emu.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Execute one QMP command.
 *
 * @param[in]  vm           Handle.
 * @param[in]  command      Command name, e.g. @c "query-status".
 * @param[in]  arguments    The @c arguments object as JSON text, or
 *                          @c NULL for a command that takes none.
 * @param[in]  timeout_ms   Timeout, ms.
 * @param[out] reply        String to append the @c return value to,
 *                          as JSON text. May be @c NULL.
 *
 * @return Status code.
 * @retval TE_EPROTO        QEMU answered with an error; its class and
 *                          description are in the log.
 * @retval TE_ECONNREFUSED  Nothing is listening on the socket.
 */
extern te_errno tapi_emu_qmp(tapi_emu_vm *vm, const char *command,
                             const char *arguments, int timeout_ms,
                             te_string *reply);

/**
 * Run a command in the human monitor, through QMP.
 *
 * The monitor a person types into. Some things have never been given
 * a QMP command of their own - saving a machine's state is one - and
 * this is how they are reached. What comes back is the text a person
 * would have seen, line endings and all.
 *
 * @param[in]  vm           Handle.
 * @param[in]  command      The command line, e.g. @c "info block".
 * @param[in]  timeout_ms   Timeout, ms.
 * @param[out] text         String to append the output to, or @c NULL.
 *
 * @return Status code.
 */
extern te_errno tapi_emu_hmp(tapi_emu_vm *vm, const char *command,
                             int timeout_ms, te_string *text);

/**
 * Pull one string field out of a QMP reply.
 *
 * @param[in]  reply    Reply text.
 * @param[in]  key      Field name.
 * @param[out] dest     String to append the value to.
 *
 * @return @c true when the field was there.
 */
extern bool tapi_emu_qmp_str(const char *reply, const char *key,
                             te_string *dest);

/**
 * Pull one boolean field out of a QMP reply.
 *
 * @param[in]  reply    Reply text.
 * @param[in]  key      Field name.
 * @param[out] value    Where to put it.
 *
 * @return @c true when the field was there.
 */
extern bool tapi_emu_qmp_bool(const char *reply, const char *key,
                              bool *value);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* !__TSF_TAPI_EMU_QMP_H__ */

/**@} <!-- END tapi_emu_qmp --> */
