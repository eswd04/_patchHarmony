# droid_lkm

out of tree kernel modules that give a stock GKI 6.12 kernel the namespace and
IPC features container runtimes expect. android16-6.12 ships with
CONFIG_SYSVIPC, CONFIG_POSIX_MQUEUE, CONFIG_PID_NS and CONFIG_IPC_NS turned
off, so a container tool like Droidspaces cannot start anything at all. this
project supplies those features from loadable modules, without recompiling the
kernel and without moving a single struct member, so prebuilt vendor modules
keep working.

this repository is the `Kern/` submodule of the Droidspaces fork, published as
`git@github.com:Dere3046/_patchHarmony.git`.

This fork is maintained at [eswd04/_patchHarmony](https://github.com/eswd04/_patchHarmony).

three modules are built:

- `droid_lkm.ko` namespace and IPC support
- `droid_lkm_compat.ko` vendor module quick fixups
- `droid_lkm_misc.ko` the features this device kernel has compiled out that a
  container still expects: runtime registered xt matches, a user namespace that
  can be unshared, the per task id map files, and the control plane below

## what it provides

- fake pid and ipc namespaces: `unshare`, `clone`, `clone3` and `setns` for
  CLONE_NEWPID and CLONE_NEWIPC, with per namespace lifetime handling
- ported SysV IPC: the msg, sem and shm syscalls plus their per namespace
  sysctls
- ported POSIX mqueue: six mq syscalls, mqueuefs and its per namespace sysctls
- `/proc/<pid>/ns/pid` and `/proc/<pid>/ns/ipc`, entries a kernel with those
  namespaces compiled out never creates
- `NSpid`, `NStgid`, `NSpgid` and `NSsid` lines in `/proc/<pid>/status`
- vendor quick fixups in the second module: a ghost task handed out when a
  vendor module asks `find_task_by_vpid` for a pid that only exists inside a
  container namespace, and a bounded pid value on that task for vendor code
  that indexes arrays by `p->pid`
- the third module's surface: `/proc/<pid>/{uid_map,gid_map,setgroups}` and
  `/proc/droid_lkm_misc/{status,version}`, so a userspace can ask what is
  installed instead of probing for behaviour it may not get

## requirements

- ARM64 device running GKI 6.12
- a way to load out of tree modules, for example a root solution or KernelPatch
- Docker, for the DDK build container
- the library set pinned in `deps.lst`, fetched by the LKM SDK

## build

	scripts/fetch-deps.sh
	scripts/build-ddkk.sh android16-6.12

`fetch-deps.sh` clones the SDK at the revision in `.sdk-version` and vendors
every library listed in `deps.lst` below `deps/`. `build-ddkk.sh <target>` runs
the DDK container and writes the target modules plus their build tree into
`out/<target>/`. library sources are never committed here, `deps.lst` pins the
exact revision of each one.

`android15-6.6` is a supported target for the main namespace/IPC module;
the OnePlus 15 User NS compatibility module targets 6.12.

## usage

	insmod droid_lkm.ko
	insmod droid_lkm_compat.ko
	insmod droid_lkm_misc.ko

load `droid_lkm.ko` before any container starts, then `droid_lkm_compat.ko` for
the vendor fixups, then `droid_lkm_misc.ko`. With `userns=1`, the third module
resolves and pins the namespace registration interface provided by the first;
the main module must already be loaded.
every module resolves its kernel symbols at load time and reports anything it
cannot find; a missing core symbol aborts the load instead of leaving a half
installed hook behind.

module parameters cover the usual cases: `mqueue`, `gate`, `verbose`,
`skip_sysvipc` and `no_fake_ns` on the first module, `skip_do_exit` on its pid
namespace part, `ghost` and `ghost_match` on the second, `xt`, `userns` and
`captrace` and `devtmpfs` on the third, plus a write only `release` described
below. each one is documented in its own `MODULE_PARM_DESC`.
`ghost_match` selects vendor modules by name prefix and defaults to `oplus_`,
`*` matches every caller. `captrace` logs every capability decision the kernel
refuses to a container task, with the function that asked for it.

## vendor compatibility

`droid_lkm.ko` is vendor neutral, everything it provides comes from the kernel
side and behaves the same on any device.

`droid_lkm_compat.ko` exists for OPPO and OnePlus kernels only. their vendor
modules call `find_task_by_vpid` and use the returned task without checking it,
and they index per pid arrays with `p->pid`. a pid that only exists inside a
container namespace breaks both assumptions, which ends in a null dereference or
an out of bounds access. the module answers those callers with a substitute task
that carries a bounded pid, selected by module name prefix (`oplus_` by default,
`*` matches every caller).

When `ghost=1` (the default), initialization now fails if the entry probe or
hook installation fails, or if `inline_hook=0`. A successful load must provide
the vendor fixup. See [OnePlus 15 reboot investigation](docs/oneplus15-compat-reboot.md)
for the regression in `513d40a` and the matching local build.

Xiaomi devices do not install this module: MIUI and HyperOS vendor modules do not
rely on that pattern, so the compat half is not needed there.

## known limits

- the user namespace supports root-only full identity mappings; see
  [OnePlus 15 User NS](docs/oneplus15-userns.md) for the supported interface
- no idmapped mount lens, so a non identity mount cannot be permission checked
- no cgroup pids or device controllers
- no nftables match set, and only the addrtype xt match is ported so far
- `NSpid` and its siblings are printed at the end of `/proc/<pid>/status`
- devtmpfs' superblock holds a reference to the module, so `rmmod` is refused
  while any mount of it is alive, which is what keeps a lazily unmounted
  superblock from being torn down after the code is gone. write `1` to
  `/sys/module/droid_lkm_misc/parameters/release` to drop the internal mount
  before unloading, the same discipline as unmounting before `rmmod`
- User NS objects, cached proc entries and nsfs operations retain the misc
  module until reboot once published; the main module is pinned as its
  dependency. At most 1024 User NS objects are retained. This replaces the
  previous unsafe free-on-unload behavior.
- `droid_lkm_misc.ko` currently builds only for 6.12; the earlier targets
  continue to build the main module and compatible optional vendor module.
- map files support direct lookup/open; the host proc directory's compiled
  entry list does not enumerate these extra files.

## credits

- [Droidspaces](https://github.com/ravindu644/Droidspaces-OSS) by ravindu644,
  the container runtime this work exists for
- [KMSDK](https://github.com/Dere3046/KMSDK), the library SDK that fetches and
  vendors the dependencies
- [KallRecon](https://github.com/Dere3046/KallRecon), kernel symbol resolution
- [Type_info](https://github.com/Dere3046/Type_info), runtime struct layout
- [KernCall](https://github.com/Dere3046/KernCall), syscall table patching
- [HooKern](https://github.com/Dere3046/HooKern), inline hook and kprobe engine

## license

GPL-2.0
