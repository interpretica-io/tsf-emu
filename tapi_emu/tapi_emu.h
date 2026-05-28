/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2026 Interpretica Unipessoal Lda */
/** @file
 * @brief Virtual machines from a test
 *
 * @defgroup tapi_emu Emulators (tapi_emu)
 * @{
 *
 * Start a machine on a Test Agent, drive it while it runs, roll it
 * back, and take it down.
 *
 * @section tapi_emu_how A job to start it, QMP to drive it
 *
 * Both, and each for what it is for. The two are not alternatives:
 *
 * **QEMU is a process**, so something has to execute it, and on a Test
 * Agent that something is @ref tapi_job. There is no library that
 * avoids this - libvirt does the same thing behind a daemon. Every
 * option that decides what the machine *is* - its memory, its disks,
 * its network - is a command line argument and can only be given at
 * that moment.
 *
 * **A command line cannot drive a machine that is already running.**
 * Everything afterwards - pause it, resume it, reset it, ask what it
 * is doing, take a snapshot, go back to one, add a disk, press a key,
 * take a picture of the screen - is @ref tapi_emu_qmp, the JSON
 * protocol QEMU listens to on a socket. That is the library half, and
 * it is where the useful part of this module lives.
 *
 * So: @ref tapi_emu_start builds the command line and runs it as a
 * job, and waits until QMP answers before returning. After that the
 * command line is history and everything goes through QMP.
 *
 * @section tapi_emu_why What this is for
 *
 * A virtual machine is worth having in a test suite for three reasons,
 * and only the first is obvious.
 *
 * - **A machine you are allowed to break.** A kernel probe that panics
 *   the host is a bad afternoon; one that panics a guest is a test
 *   result. tsf-kernel's module tests belong in here.
 * - **A machine that goes back.** @ref tapi_emu_snapshot_save and
 *   @ref tapi_emu_snapshot_load put the guest back exactly as it was,
 *   in a second and without rebooting. A suite that has to install
 *   something before each test can install it once.
 * - **A second host on demand.** A test that needs two machines to
 *   talk to each other needs two machines. With user networking and a
 *   forwarded port, the guest can be reached from the agent - and a
 *   guest reachable over SSH is a guest that can be a Test Agent of
 *   its own.
 *
 * @code
 * tapi_emu_opt opt = tapi_emu_default_opt;
 * tapi_emu_vm *vm = NULL;
 *
 * opt.memory_mb = 512;
 * opt.drives = (const char *[]){ "file=/srv/images/dut.qcow2,if=virtio" };
 * opt.n_drives = 1;
 * opt.hostfwd_ssh_port = 2222;
 *
 * CHECK_RC(tapi_emu_start(factory, &opt, 60000, &vm));
 * CHECK_RC(tapi_emu_wait_port(vm, 2222, 120000));
 * ... the guest is up and reachable on the agent's port 2222 ...
 * CLEANUP_CHECK_RC(tapi_emu_stop(vm, 10000));
 * @endcode
 *
 * @note It needs @c qemu-system-* on the agent, and @c python3 as
 *       well: the QMP socket lives on the agent, so something on the
 *       agent has to speak to it. See @ref tapi_emu_qmp.
 */

#ifndef __TSF_TAPI_EMU_H__
#define __TSF_TAPI_EMU_H__

#include "te_defs.h"
#include "te_errno.h"
#include "te_string.h"
#include "tapi_job.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Default timeout for one operation on a machine, ms. */
#define TAPI_EMU_TIMEOUT_MS 60000

/** What a machine is doing. */
typedef enum tapi_emu_state {
    /** It is not there, or it has gone. */
    TAPI_EMU_GONE = 0,
    /** Built but not started, because @a paused was asked for. */
    TAPI_EMU_PRELAUNCH,
    /** Running. */
    TAPI_EMU_RUNNING,
    /** Stopped by @ref tapi_emu_pause or by the guest. */
    TAPI_EMU_PAUSED,
    /** The guest has shut itself down. */
    TAPI_EMU_SHUTDOWN,
    /** QEMU said something this library does not know a name for. */
    TAPI_EMU_OTHER,
} tapi_emu_state;

/** How to build the machine. */
typedef struct tapi_emu_opt {
    /**
     * The binary; @c NULL means @c "qemu-system-x86_64".
     *
     * The architecture is part of the name, so this is also how a test
     * asks for a machine that is not the agent's own architecture -
     * which is the whole point of an emulator and is much slower.
     */
    const char *binary;
    /** @c -machine; @c NULL means QEMU's default for the binary. */
    const char *machine;
    /** @c -cpu; @c NULL means QEMU's default. */
    const char *cpu;
    /**
     * @c -accel; @c NULL means let QEMU choose.
     *
     * @c "kvm" on Linux, @c "hvf" on macOS, @c "whpx" on Windows,
     * @c "tcg" to emulate. Asking for one the agent cannot give is a
     * failure to start, which is better than silently running fifty
     * times slower.
     */
    const char *accel;
    /** Number of processors; @c 0 means one. */
    unsigned int smp;
    /** Memory in megabytes; @c 0 means QEMU's default. */
    unsigned int memory_mb;

    /** @c -drive arguments, one per drive. */
    const char **drives;
    /** Number of @a drives. */
    size_t n_drives;
    /** @c -netdev arguments. */
    const char **netdevs;
    /** Number of @a netdevs. */
    size_t n_netdevs;
    /** @c -device arguments. */
    const char **devices;
    /** Number of @a devices. */
    size_t n_devices;

    /** @c -kernel, to boot one directly without a bootloader. */
    const char *kernel;
    /** @c -initrd. */
    const char *initrd;
    /** @c -append, the kernel command line. */
    const char *append;
    /** @c -cdrom. */
    const char *cdrom;

    /**
     * Give the guest user networking with this host port forwarded to
     * its port 22; @c 0 for none.
     *
     * The short way to a guest that can be reached. It adds a
     * @c -netdev and a @c -device, so a test that wants a different
     * network should leave this at @c 0 and use @a netdevs.
     */
    uint16_t hostfwd_ssh_port;

    /**
     * Discard everything the guest writes to its disks.
     *
     * @c -snapshot. The image is left exactly as it was, which is what
     * a suite that runs against a prepared image wants, and it means
     * two tests can use one image at the same time.
     */
    bool discard_writes;
    /** Start stopped, so a test can set something up first (@c -S). */
    bool paused;
    /** Where to put the QMP socket on the agent; @c NULL to generate. */
    const char *qmp_socket;
    /** Extra arguments, for what this does not wrap. */
    const char **extra_args;
    /** Number of @a extra_args. */
    size_t n_extra_args;
} tapi_emu_opt;

/** Defaults: no display, no disks, QMP on a generated socket. */
extern const tapi_emu_opt tapi_emu_default_opt;

/** A running machine. */
typedef struct tapi_emu_vm tapi_emu_vm;

/**
 * Is there a QEMU on the agent?
 *
 * @param factory       Job factory.
 * @param binary        The binary to look for, or @c NULL for the
 *                      default.
 * @param timeout_ms    Timeout, ms.
 *
 * @return @c true when it answers.
 */
extern bool tapi_emu_available(tapi_job_factory_t *factory,
                               const char *binary, int timeout_ms);

/**
 * Start a machine and wait until it can be driven.
 *
 * It does not return before QMP answers. A start that returned early
 * would leave the very next command racing the machine, and the
 * command would usually win.
 *
 * @param[in]  factory      Job factory; QEMU runs on this agent.
 * @param[in]  opt          What machine to build.
 * @param[in]  timeout_ms   How long to give it to come up, ms.
 * @param[out] vm           Handle; release with tapi_emu_stop().
 *
 * @return Status code.
 */
extern te_errno tapi_emu_start(tapi_job_factory_t *factory,
                               const tapi_emu_opt *opt, int timeout_ms,
                               tapi_emu_vm **vm);

/**
 * Ask the machine what it is doing.
 *
 * @param[in]  vm           Handle.
 * @param[in]  timeout_ms   Timeout, ms.
 * @param[out] state        Where to put it.
 *
 * @return Status code.
 */
extern te_errno tapi_emu_state_get(tapi_emu_vm *vm, int timeout_ms,
                                   tapi_emu_state *state);

/**
 * Stop the machine where it stands, keeping its memory.
 *
 * @param vm            Handle.
 * @param timeout_ms    Timeout, ms.
 *
 * @return Status code.
 */
extern te_errno tapi_emu_pause(tapi_emu_vm *vm, int timeout_ms);

/**
 * Let it carry on.
 *
 * @param vm            Handle.
 * @param timeout_ms    Timeout, ms.
 *
 * @return Status code.
 */
extern te_errno tapi_emu_resume(tapi_emu_vm *vm, int timeout_ms);

/**
 * Reset the machine, as the reset button would.
 *
 * @param vm            Handle.
 * @param timeout_ms    Timeout, ms.
 *
 * @return Status code.
 */
extern te_errno tapi_emu_reset(tapi_emu_vm *vm, int timeout_ms);

/**
 * Ask the guest to shut itself down, as the power button would.
 *
 * This is a request. A guest with no ACPI daemon ignores it, which is
 * why tapi_emu_stop() does not wait for ever.
 *
 * @param vm            Handle.
 * @param timeout_ms    Timeout, ms.
 *
 * @return Status code.
 */
extern te_errno tapi_emu_powerdown(tapi_emu_vm *vm, int timeout_ms);

/**
 * Save the machine's whole state under a name.
 *
 * Memory, devices and disks, in a second, without rebooting. It needs
 * a qcow2 disk to write into - @c -snapshot and a raw image both make
 * this fail - and it does not work while @a discard_writes is on.
 *
 * @param vm            Handle.
 * @param name          Name for the snapshot.
 * @param timeout_ms    Timeout, ms.
 *
 * @return Status code.
 */
extern te_errno tapi_emu_snapshot_save(tapi_emu_vm *vm, const char *name,
                                       int timeout_ms);

/**
 * Put the machine back to a saved state.
 *
 * The guest does not notice: from inside, no time has passed. This is
 * what lets a suite prepare a machine once and start every test from
 * the same place.
 *
 * @param vm            Handle.
 * @param name          Name given to tapi_emu_snapshot_save().
 * @param timeout_ms    Timeout, ms.
 *
 * @return Status code.
 */
extern te_errno tapi_emu_snapshot_load(tapi_emu_vm *vm, const char *name,
                                       int timeout_ms);

/**
 * Forget a saved state.
 *
 * @param vm            Handle.
 * @param name          Its name.
 * @param timeout_ms    Timeout, ms.
 *
 * @return Status code.
 */
extern te_errno tapi_emu_snapshot_delete(tapi_emu_vm *vm, const char *name,
                                         int timeout_ms);

/**
 * Wait until something answers on a port of the agent.
 *
 * For a guest that has just been started with @a hostfwd_ssh_port: the
 * machine is up long before the operating system in it is, and this is
 * the difference between the two.
 *
 * @param vm            Handle, for the log and for the agent.
 * @param port          Port on the agent.
 * @param timeout_ms    How long to keep trying, ms.
 *
 * @return Status code.
 * @retval TE_ETIMEDOUT Nothing answered in time.
 */
extern te_errno tapi_emu_wait_port(tapi_emu_vm *vm, uint16_t port,
                                   int timeout_ms);

/**
 * Take a picture of the machine's screen.
 *
 * @param vm            Handle.
 * @param path          Where to write it on the agent, as PPM.
 * @param timeout_ms    Timeout, ms.
 *
 * @return Status code.
 * @retval TE_ENOENT    The machine has no console to photograph, which
 *                      a machine built with no display does not.
 */
extern te_errno tapi_emu_screendump(tapi_emu_vm *vm, const char *path,
                                    int timeout_ms);

/**
 * What the machine printed on its serial console so far.
 *
 * @param[in]  vm           Handle.
 * @param[out] text         String to append it to.
 *
 * @return Status code.
 */
extern te_errno tapi_emu_console(tapi_emu_vm *vm, te_string *text);

/**
 * Stop the machine and release the handle.
 *
 * Asks the guest to shut down, gives it a moment, and then takes the
 * machine away. Safe on @c NULL and safe to call from a cleanup
 * section.
 *
 * @param vm            Handle.
 * @param timeout_ms    How long to let the guest finish, ms.
 *
 * @return Status code.
 */
extern te_errno tapi_emu_stop(tapi_emu_vm *vm, int timeout_ms);

/**
 * The job factory the machine was started through.
 *
 * @param vm            Handle.
 *
 * @return The factory.
 */
extern tapi_job_factory_t *tapi_emu_factory(const tapi_emu_vm *vm);

/**
 * Where the machine's QMP socket is, on the agent.
 *
 * @param vm            Handle.
 *
 * @return The path, never @c NULL.
 */
extern const char *tapi_emu_qmp_socket(const tapi_emu_vm *vm);

/**
 * Spell out a state.
 *
 * @param state         State.
 *
 * @return A static string, never @c NULL.
 */
extern const char *tapi_emu_state2str(tapi_emu_state state);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* !__TSF_TAPI_EMU_H__ */

/**@} <!-- END tapi_emu --> */
