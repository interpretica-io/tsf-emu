# tsf-emu

Virtual machines from a test suite, packaged as an external Test
Environment (TE) repository.

Library:

- `tapi_emu` — engine-side, built as a shared library: start a machine
  on a Test Agent, drive it over QMP while it runs, roll it back to a
  snapshot, and take it down.

## A job to start it, QMP to drive it

Both, and each for what it is for. They are not alternatives, and this
is the question worth settling before reading anything else.

**QEMU is a process**, so something has to execute it, and on a Test
Agent that something is `tapi_job`. No library avoids this — libvirt
does the same thing behind a daemon. Every option that decides what the
machine *is* — its memory, its disks, its network — is a command line
argument and can only be given at that one moment.

**A command line cannot drive a machine that is already running.**
Everything afterwards — pause, resume, reset, ask what it is doing,
take a snapshot, go back to one, hotplug a disk, take a picture of the
screen — is **QMP**, the JSON protocol QEMU listens to on a socket.
That is the library half, and it is where the useful part of this
module lives.

So `tapi_emu_start()` builds the command line, runs it as a job, and
waits until QMP answers before returning. After that the command line
is history.

## What a virtual machine is for in a test suite

Three reasons, and only the first is obvious.

- **A machine you are allowed to break.** A kernel probe that panics
  the host is a bad afternoon; one that panics a guest is a test
  result. The module tests of
  [tsf-kernel](https://github.com/interpretica-io/tsf-kernel) belong in
  here.
- **A machine that goes back.** `tapi_emu_snapshot_save()` and
  `tapi_emu_snapshot_load()` put the guest back exactly as it was, in a
  second and without rebooting — memory, devices and disks. A suite
  that has to install something before each test can install it once.
- **A second host on demand.** A test that needs two machines needs two
  machines. With user networking and a forwarded port the guest is
  reachable from the agent, and a guest reachable over SSH is a guest
  that can be a Test Agent of its own.

## Usage

```yaml
repositories:
  - name: tsf_devtool
    url: https://github.com/interpretica-io/tsf-devtool.git
    ref: <tag>
    libs: [ tapi_devtool ]
  - name: tsf_emu
    url: https://github.com/interpretica-io/tsf-emu.git
    ref: <tag>
    libs: [ tapi_emu ]
```

```
TE_EXT_REPO_USE([tsf_devtool], [], [tapi_devtool])
TE_EXT_REPO_USE([tsf_emu], [], [tapi_emu])
```

Then add `tapi_emu` to `te_libs` in the suite's `meson.build`.
Requires TE with `TE_EXT_REPO` support and an **RPC** job factory. On
the agent it needs `qemu-system-*` and `python3`.

```c
tapi_emu_opt opt = tapi_emu_default_opt;
tapi_emu_vm *vm = NULL;

opt.memory_mb = 512;
opt.drives = (const char *[]){ "file=/srv/images/dut.qcow2,if=virtio" };
opt.n_drives = 1;
opt.hostfwd_ssh_port = 2222;

CHECK_RC(tapi_emu_start(factory, &opt, 60000, &vm));
CHECK_RC(tapi_emu_wait_port(vm, 2222, 120000));
/* the guest is up and reachable on the agent's port 2222 */
CLEANUP_CHECK_RC(tapi_emu_stop(vm, 10000));
```

## How QMP reaches the agent

The socket is on the agent, so something on the agent has to speak to
it. That something is a small `python3` script this library writes
there once and runs per command: connect, handshake, send one command,
print the reply, exit.

One process per command, deliberately. A persistent channel would be
faster and would have to be kept alive across a test that fails half
way; QMP commands are not in hot loops, and a stateless helper cannot
leave anything behind.

Three things in that helper are not obvious, and all three were
measured against QEMU 10.2.0 rather than assumed:

- **The greeting comes first, unasked.** QEMU sends
  `{"QMP": {"version": ..., "capabilities": ["oob"]}}` the moment
  anything connects, and it has to be read before anything can be sent.
- **Nothing works until the handshake.** `qmp_capabilities` must be
  sent and answered first.
- **Events are not replies.** QEMU sends events down the same socket
  whenever it likes, in between everything else — a `quit` gets a
  `SHUTDOWN` event *before* its own answer. A client that reads one
  line and calls it the answer will sooner or later read an event
  instead and report nonsense. Anything with an `event` key is skipped.

## Starting waits for QMP, not for the process

QEMU exists long before its socket does, and a command sent in that gap
is refused — which reads as a broken machine rather than a slow one.

Asking QMP is also the only honest test that the machine came up at
all: QEMU writes a complaint to standard error and exits when it does
not like its arguments, and a job would happily report that it had
started something. So the wait loop asks QMP and, between attempts,
checks whether QEMU has given up — and if it has, the reason from
standard error goes into the log instead of a timeout.

## Snapshots

`savevm`, `loadvm` and `delvm` never got a QMP command of their own, so
they go through `human-monitor-command`. The newer `snapshot-save` and
`snapshot-load` jobs exist but are asynchronous and want a job id and a
device list, which is more ceremony than a test that just wants to go
back needs.

The monitor reports a refusal **as text inside a successful reply**:

```
{"return": "Error: no block device can store vmstate for snapshot\r\n"}
```

So the reply has to be read, not just the status. Measured — that is
exactly what a `savevm` gets on a machine with no qcow2 to write into,
and without reading it the test would believe it had a checkpoint.

Snapshots need a qcow2 disk. A raw image cannot hold one, and
`discard_writes` (`-snapshot`) makes it fail too — the writes it would
save are the ones being discarded.

## What was verified

Everything here was run against QEMU 10.2.0, not written from the
documentation:

- **The QMP helper**, exactly as it is embedded in the source:
  `query-status` before and after `cont` and `stop`,
  `human-monitor-command` with arguments, an unknown command coming
  back as `{"error": {"class": "CommandNotFound", ...}}`, and both
  failure paths — no socket there, and the socket gone after `quit` —
  exiting 2 as the C side expects.
- **The generated command lines**, replayed as the library builds
  them. The default options start a machine that answers QMP; adding
  memory and `hostfwd_ssh_port` starts one whose forwarded port is
  really listening, which is what `tapi_emu_wait_port()` looks for.
- **Snapshots**, end to end on a qcow2 disk: `savevm` returning an
  empty string for success, `info snapshots` listing the checkpoint,
  `loadvm`, `delvm`, and the `Error:` text that comes back when there
  is nowhere to store one.

What has **not** been exercised is a machine with an operating system
in it: every check above used a machine with no guest. The paths that
depend on one — `wait_port` actually reaching an SSH server,
`powerdown` being obeyed, the serial console carrying a boot log — are
built on pieces that were verified and have not been run together.

## Scope

- **One machine per handle**, started by the test and stopped by it.
  `tapi_emu_stop()` asks the guest to shut down, waits, and then takes
  the machine away — a guest with no ACPI daemon ignores the request,
  which is why the wait has an end.
- **User networking by default.** It needs no privileges and no bridge,
  the guest can reach out, and one port of the agent reaches back. It
  is slow and unreachable from anywhere but the agent, which is the
  right trade for a test. A suite that needs real networking should
  leave `hostfwd_ssh_port` at `0` and pass its own `netdevs`.
- **`-nodefaults` always.** Without it QEMU adds a pile of devices
  nobody asked for, and a test that thought it knew what was in the
  machine did not.
- **libvirt is not a backend here.** It could be one — the same
  lifecycle over `virsh` — and would suit a lab whose machines are
  already managed that way. QMP was the one to build first because it
  is what a machine this library started can be driven with, with
  nothing else installed.
