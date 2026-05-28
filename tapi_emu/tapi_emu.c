/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief Virtual machines from a test
 *
 * The command line is built here and used once. Everything after the
 * machine has started goes through QMP, because a command line cannot
 * talk to a process that is already running.
 */

#define TE_LGR_USER "TAPI EMU"

#include "te_config.h"

#include <stdlib.h>
#include <string.h>

#include "logger_api.h"
#include "tapi_cfg_base.h"
#include "tapi_file.h"
#include "te_alloc.h"
#include "te_sleep.h"
#include "te_str.h"
#include "te_string.h"
#include "te_vector.h"

#include "tapi_emu.h"
#include "tapi_emu_qmp.h"
#include "tapi_emu_internal.h"

/** How often the machine is asked whether QMP answers yet, ms. */
#define EMU_READY_POLL_MS 250

/** How long to give the guest to shut down before taking it away, ms. */
#define EMU_SHUTDOWN_GRACE_MS 10000

/** How often a port is tried while waiting for a guest, ms. */
#define EMU_PORT_POLL_MS 1000

const tapi_emu_opt tapi_emu_default_opt = {
    .binary = NULL,
};

/* See description in tapi_emu.h */
const char *
tapi_emu_state2str(tapi_emu_state state)
{
    switch (state)
    {
        case TAPI_EMU_PRELAUNCH:
            return "prelaunch";
        case TAPI_EMU_RUNNING:
            return "running";
        case TAPI_EMU_PAUSED:
            return "paused";
        case TAPI_EMU_SHUTDOWN:
            return "shutdown";
        case TAPI_EMU_OTHER:
            return "other";
        default:
            return "gone";
    }
}

/** The binary to run. */
static const char *
emu_binary(const tapi_emu_opt *opt)
{
    return opt->binary != NULL ? opt->binary : "qemu-system-x86_64";
}

/* See description in tapi_emu.h */
bool
tapi_emu_available(tapi_job_factory_t *factory, const char *binary,
                   int timeout_ms)
{
    te_vec args = TE_VEC_INIT(char *);
    te_string out = TE_STRING_INIT;
    const char *program = binary != NULL ? binary : "qemu-system-x86_64";
    int code = 0;
    bool ok;

    tapi_emu_arg(&args, "--version");

    ok = tapi_emu_cmd(factory, "qemu", program, &args, timeout_ms, &out,
                      NULL, &code) == 0 && code == 0;

    if (ok)
    {
        char *line = te_str_strip_spaces(te_string_value(&out));

        if (line != NULL)
        {
            RING("The agent has %s", line);
            free(line);
        }
    }
    else
    {
        RING("There is no %s on the agent", program);
    }

    te_vec_deep_free(&args);
    te_string_free(&out);

    return ok;
}

/** Build the QEMU command line. */
static void
emu_build_args(const tapi_emu_opt *opt, const char *qmp_socket,
               te_vec *args)
{
    size_t i;

    /*
     * -nodefaults first: without it QEMU adds a pile of devices nobody
     * asked for, and a test that thought it knew what was in the
     * machine did not.
     */
    tapi_emu_arg(args, "-nodefaults");
    tapi_emu_arg(args, "-display");
    tapi_emu_arg(args, "none");

    /*
     * The serial console onto QEMU's own output, which is the job's
     * output, which is the log. A guest that fails to boot says why
     * here and nowhere else.
     */
    tapi_emu_arg(args, "-serial");
    tapi_emu_arg(args, "stdio");

    tapi_emu_arg(args, "-qmp");
    tapi_emu_arg(args, "unix:%s,server,nowait", qmp_socket);

    if (opt->machine != NULL)
    {
        tapi_emu_arg(args, "-machine");
        tapi_emu_arg(args, "%s", opt->machine);
    }
    if (opt->cpu != NULL)
    {
        tapi_emu_arg(args, "-cpu");
        tapi_emu_arg(args, "%s", opt->cpu);
    }
    if (opt->accel != NULL)
    {
        tapi_emu_arg(args, "-accel");
        tapi_emu_arg(args, "%s", opt->accel);
    }
    if (opt->smp != 0)
    {
        tapi_emu_arg(args, "-smp");
        tapi_emu_arg(args, "%u", opt->smp);
    }
    if (opt->memory_mb != 0)
    {
        tapi_emu_arg(args, "-m");
        tapi_emu_arg(args, "%u", opt->memory_mb);
    }

    for (i = 0; i < opt->n_drives; i++)
    {
        tapi_emu_arg(args, "-drive");
        tapi_emu_arg(args, "%s", opt->drives[i]);
    }
    for (i = 0; i < opt->n_netdevs; i++)
    {
        tapi_emu_arg(args, "-netdev");
        tapi_emu_arg(args, "%s", opt->netdevs[i]);
    }
    for (i = 0; i < opt->n_devices; i++)
    {
        tapi_emu_arg(args, "-device");
        tapi_emu_arg(args, "%s", opt->devices[i]);
    }

    if (opt->hostfwd_ssh_port != 0)
    {
        /*
         * User networking: no privileges needed, no bridge to set up,
         * and the guest can reach the world while one port of the
         * agent reaches back. It is slow and it cannot be reached from
         * anywhere but the agent, which is exactly the right trade for
         * a test.
         */
        tapi_emu_arg(args, "-netdev");
        tapi_emu_arg(args, "user,id=tapi_net0,hostfwd=tcp::%u-:22",
                     opt->hostfwd_ssh_port);
        tapi_emu_arg(args, "-device");
        tapi_emu_arg(args, "virtio-net-pci,netdev=tapi_net0");
    }

    if (opt->kernel != NULL)
    {
        tapi_emu_arg(args, "-kernel");
        tapi_emu_arg(args, "%s", opt->kernel);
    }
    if (opt->initrd != NULL)
    {
        tapi_emu_arg(args, "-initrd");
        tapi_emu_arg(args, "%s", opt->initrd);
    }
    if (opt->append != NULL)
    {
        tapi_emu_arg(args, "-append");
        tapi_emu_arg(args, "%s", opt->append);
    }
    if (opt->cdrom != NULL)
    {
        tapi_emu_arg(args, "-cdrom");
        tapi_emu_arg(args, "%s", opt->cdrom);
    }

    if (opt->discard_writes)
        tapi_emu_arg(args, "-snapshot");
    if (opt->paused)
        tapi_emu_arg(args, "-S");

    for (i = 0; i < opt->n_extra_args; i++)
        tapi_emu_arg(args, "%s", opt->extra_args[i]);
}

/** Release a machine handle without touching the machine. */
static void
emu_free(tapi_emu_vm *vm)
{
    if (vm == NULL)
        return;

    if (vm->helper != NULL && vm->ta != NULL)
        (void)tapi_file_ta_unlink_fmt(vm->ta, "%s", vm->helper);

    free(vm->ta);
    free(vm->qmp_socket);
    free(vm->helper);
    free(vm->python);
    free(vm);
}

/* See description in tapi_emu.h */
te_errno
tapi_emu_start(tapi_job_factory_t *factory, const tapi_emu_opt *opt,
               int timeout_ms, tapi_emu_vm **vm)
{
    const char *ta = tapi_job_factory_ta(factory);
    te_vec args = TE_VEC_INIT(char *);
    tapi_emu_vm *result;
    int waited_ms;
    te_errno rc;

    if (ta == NULL)
    {
        ERROR("Cannot determine the agent behind the job factory");
        return TE_RC(TE_TAPI, TE_EINVAL);
    }

    result = TE_ALLOC(sizeof(*result));
    result->run = (tapi_devtool_run)TAPI_DEVTOOL_RUN_INIT;
    result->factory = factory;
    result->ta = TE_STRDUP(ta);
    result->python = TE_STRDUP("python3");

    if (opt->qmp_socket != NULL)
    {
        result->qmp_socket = TE_STRDUP(opt->qmp_socket);
    }
    else
    {
        te_string path = TE_STRING_INIT;
        char *tmp_dir = tapi_cfg_base_get_ta_dir(ta,
                                                 TAPI_CFG_BASE_TA_DIR_TMP);

        if (tmp_dir == NULL)
        {
            ERROR("Failed to get the temporary directory of TA %s", ta);
            emu_free(result);
            return TE_RC(TE_TAPI, TE_EFAIL);
        }

        tapi_file_make_custom_pathname(&path, tmp_dir, "-qmp.sock");
        free(tmp_dir);
        result->qmp_socket = path.ptr;
    }

    emu_build_args(opt, result->qmp_socket, &args);

    rc = tapi_emu_spawn(factory, "qemu", emu_binary(opt), &args,
                        &result->run);
    te_vec_deep_free(&args);

    if (rc != 0)
    {
        emu_free(result);
        return rc;
    }

    /*
     * Wait for QMP rather than for the process. QEMU exists long
     * before its socket does, and a command sent in that gap is
     * refused - which reads as a broken machine rather than as a slow
     * one.
     *
     * Asking QMP is also the only honest test that the machine came
     * up at all: QEMU exits with a message on stderr when it does not
     * like its arguments, and the job would happily report that it had
     * started something.
     */
    if (timeout_ms <= 0)
        timeout_ms = TAPI_EMU_TIMEOUT_MS;

    for (waited_ms = 0; waited_ms < timeout_ms;
         waited_ms += EMU_READY_POLL_MS)
    {
        tapi_emu_state state;

        if (tapi_emu_state_get(result, EMU_READY_POLL_MS * 8, &state) == 0)
        {
            RING("The machine is up and %s, QMP on %s",
                 tapi_emu_state2str(state), result->qmp_socket);
            *vm = result;
            return 0;
        }

        /* Did QEMU give up while we were waiting? */
        rc = tapi_devtool_run_wait(&result->run, 0);
        if (TE_RC_GET_ERROR(rc) != TE_EINPROGRESS)
        {
            tapi_devtool_output output;

            tapi_devtool_run_get_output(&result->run, &output);
            ERROR("QEMU stopped instead of starting: %s",
                  output.err != NULL && output.err[0] != '\0' ?
                      output.err : "no reason given");
            tapi_devtool_run_fini(&result->run);
            emu_free(result);
            return TE_RC(TE_TAPI, TE_EFAIL);
        }

        te_motivated_msleep(EMU_READY_POLL_MS, "waiting for QMP to answer");
    }

    ERROR("The machine did not answer on QMP within %d ms", timeout_ms);
    (void)tapi_devtool_run_stop(&result->run);
    tapi_devtool_run_fini(&result->run);
    emu_free(result);

    return TE_RC(TE_TAPI, TE_ETIMEDOUT);
}

/* See description in tapi_emu.h */
te_errno
tapi_emu_state_get(tapi_emu_vm *vm, int timeout_ms, tapi_emu_state *state)
{
    te_string reply = TE_STRING_INIT;
    te_string status = TE_STRING_INIT;
    const char *text;
    te_errno rc;

    *state = TAPI_EMU_GONE;

    rc = tapi_emu_qmp(vm, "query-status", NULL, timeout_ms, &reply);
    if (rc != 0)
    {
        te_string_free(&reply);
        return rc;
    }

    if (!tapi_emu_qmp_str(te_string_value(&reply), "status", &status))
    {
        te_string_free(&reply);
        te_string_free(&status);
        return TE_RC(TE_TAPI, TE_EPROTO);
    }

    text = te_string_value(&status);

    /*
     * The words QEMU uses, measured: "prelaunch" before -S is
     * released, "running", "paused", "shutdown". There are others -
     * "inmigrate", "postmigrate", "suspended" - which a test that
     * cares about them should read from query-status itself rather
     * than have flattened into a name this library invented.
     */
    if (strcmp(text, "running") == 0)
        *state = TAPI_EMU_RUNNING;
    else if (strcmp(text, "paused") == 0)
        *state = TAPI_EMU_PAUSED;
    else if (strcmp(text, "prelaunch") == 0)
        *state = TAPI_EMU_PRELAUNCH;
    else if (strcmp(text, "shutdown") == 0)
        *state = TAPI_EMU_SHUTDOWN;
    else
        *state = TAPI_EMU_OTHER;

    te_string_free(&reply);
    te_string_free(&status);

    return 0;
}

/* See description in tapi_emu.h */
te_errno
tapi_emu_pause(tapi_emu_vm *vm, int timeout_ms)
{
    return tapi_emu_qmp(vm, "stop", NULL, timeout_ms, NULL);
}

/* See description in tapi_emu.h */
te_errno
tapi_emu_resume(tapi_emu_vm *vm, int timeout_ms)
{
    return tapi_emu_qmp(vm, "cont", NULL, timeout_ms, NULL);
}

/* See description in tapi_emu.h */
te_errno
tapi_emu_reset(tapi_emu_vm *vm, int timeout_ms)
{
    return tapi_emu_qmp(vm, "system_reset", NULL, timeout_ms, NULL);
}

/* See description in tapi_emu.h */
te_errno
tapi_emu_powerdown(tapi_emu_vm *vm, int timeout_ms)
{
    return tapi_emu_qmp(vm, "system_powerdown", NULL, timeout_ms, NULL);
}

/**
 * One of the snapshot commands.
 *
 * Through the human monitor, because that is where they are: savevm,
 * loadvm and delvm never got a QMP command of their own. The newer
 * snapshot-save and snapshot-load jobs exist but are asynchronous and
 * want a job id and a device list, which is more ceremony than a test
 * that just wants to go back needs.
 */
static te_errno
emu_snapshot(tapi_emu_vm *vm, const char *verb, const char *name,
             int timeout_ms)
{
    te_string command = TE_STRING_INIT;
    te_string output = TE_STRING_INIT;
    te_errno rc;

    te_string_append(&command, "%s %s", verb, name);

    rc = tapi_emu_hmp(vm, command.ptr, timeout_ms, &output);

    /*
     * The monitor reports a refusal as text and not as an error, so
     * the reply has to be read: "Error: ..." is what savevm says when
     * there is no qcow2 to write into, and the command still
     * "succeeded".
     */
    if (rc == 0 && output.len != 0)
    {
        const char *text = te_string_value(&output);

        if (strstr(text, "Error") != NULL || strstr(text, "error") != NULL)
        {
            ERROR("%s %s was refused: %s", verb, name, text);
            rc = TE_RC(TE_TAPI, TE_EFAIL);
        }
        else
        {
            RING("%s %s: %s", verb, name, text);
        }
    }

    te_string_free(&command);
    te_string_free(&output);

    return rc;
}

/* See description in tapi_emu.h */
te_errno
tapi_emu_snapshot_save(tapi_emu_vm *vm, const char *name, int timeout_ms)
{
    return emu_snapshot(vm, "savevm", name, timeout_ms);
}

/* See description in tapi_emu.h */
te_errno
tapi_emu_snapshot_load(tapi_emu_vm *vm, const char *name, int timeout_ms)
{
    return emu_snapshot(vm, "loadvm", name, timeout_ms);
}

/* See description in tapi_emu.h */
te_errno
tapi_emu_snapshot_delete(tapi_emu_vm *vm, const char *name, int timeout_ms)
{
    return emu_snapshot(vm, "delvm", name, timeout_ms);
}

/* See description in tapi_emu.h */
te_errno
tapi_emu_wait_port(tapi_emu_vm *vm, uint16_t port, int timeout_ms)
{
    int waited_ms;

    for (waited_ms = 0; waited_ms < timeout_ms;
         waited_ms += EMU_PORT_POLL_MS)
    {
        te_vec args = TE_VEC_INIT(char *);
        int code = 0;
        bool answered;

        /*
         * Asked from the agent, because the port is forwarded to the
         * agent's loopback and nothing outside can see it. python3 is
         * already needed for QMP, so nothing new has to be there.
         */
        tapi_emu_arg(&args, "-c");
        tapi_emu_arg(&args,
            "import socket,sys; s=socket.socket(); s.settimeout(2); "
            "sys.exit(0 if s.connect_ex(('127.0.0.1',%u))==0 else 1)",
            port);

        answered = tapi_emu_cmd(vm->factory, "wait-port", vm->python,
                                &args, EMU_PORT_POLL_MS * 8, NULL, NULL,
                                &code) == 0 && code == 0;

        te_vec_deep_free(&args);

        if (answered)
        {
            RING("The guest answered on port %u after %d ms", port,
                 waited_ms);
            return 0;
        }

        te_motivated_msleep(EMU_PORT_POLL_MS,
                            "waiting for the guest to come up");
    }

    ERROR("Nothing answered on port %u within %d ms. The machine is "
          "running; what is in it may not have finished booting.",
          port, timeout_ms);

    return TE_RC(TE_TAPI, TE_ETIMEDOUT);
}

/* See description in tapi_emu.h */
te_errno
tapi_emu_screendump(tapi_emu_vm *vm, const char *path, int timeout_ms)
{
    te_string arguments = TE_STRING_INIT;
    te_errno rc;

    te_string_append(&arguments, "{\"filename\": \"%s\"}", path);

    rc = tapi_emu_qmp(vm, "screendump", arguments.ptr, timeout_ms, NULL);

    /*
     * Measured: a machine built with -display none answers
     * "There is no console to take a screendump from". That is a
     * property of how the machine was built and not a failure to run
     * the command, so it comes back as ENOENT.
     */
    if (TE_RC_GET_ERROR(rc) == TE_EPROTO)
        rc = TE_RC(TE_TAPI, TE_ENOENT);

    te_string_free(&arguments);

    return rc;
}

/* See description in tapi_emu.h */
te_errno
tapi_emu_console(tapi_emu_vm *vm, te_string *text)
{
    tapi_devtool_output output;

    tapi_devtool_run_get_output(&vm->run, &output);

    if (output.out != NULL)
        te_string_append(text, "%s", output.out);

    return 0;
}

/* See description in tapi_emu.h */
te_errno
tapi_emu_stop(tapi_emu_vm *vm, int timeout_ms)
{
    int waited_ms;
    te_errno rc = 0;

    if (vm == NULL || vm->gone)
    {
        emu_free(vm);
        return 0;
    }

    if (timeout_ms <= 0)
        timeout_ms = EMU_SHUTDOWN_GRACE_MS;

    /*
     * Ask, wait, then take it away - what a power button does, and
     * then what pulling the plug does. A guest with no ACPI daemon
     * ignores the request entirely, so the wait has an end.
     */
    if (tapi_emu_powerdown(vm, timeout_ms) == 0)
    {
        for (waited_ms = 0; waited_ms < timeout_ms;
             waited_ms += EMU_READY_POLL_MS)
        {
            tapi_emu_state state;

            if (tapi_emu_state_get(vm, EMU_READY_POLL_MS * 4, &state) != 0)
                break;

            if (state == TAPI_EMU_SHUTDOWN || state == TAPI_EMU_GONE)
                break;

            te_motivated_msleep(EMU_READY_POLL_MS,
                                "waiting for the guest to shut down");
        }
    }

    (void)tapi_devtool_run_stop(&vm->run);
    (void)tapi_devtool_run_wait(&vm->run, timeout_ms);

    {
        tapi_devtool_output output;

        tapi_devtool_run_get_output(&vm->run, &output);
        if (output.out != NULL && output.out[0] != '\0')
            RING("The machine's console said:\n%s", output.out);
    }

    rc = tapi_devtool_run_fini(&vm->run);

    /* The socket is QEMU's; it does not always take it with it. */
    (void)tapi_file_ta_unlink_fmt(vm->ta, "%s", vm->qmp_socket);

    vm->gone = true;
    emu_free(vm);

    return rc;
}

/* See description in tapi_emu.h */
tapi_job_factory_t *
tapi_emu_factory(const tapi_emu_vm *vm)
{
    return vm->factory;
}

/* See description in tapi_emu.h */
const char *
tapi_emu_qmp_socket(const tapi_emu_vm *vm)
{
    return vm->qmp_socket;
}
