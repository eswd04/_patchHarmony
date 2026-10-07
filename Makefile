obj-m := droid_lkm.o

KDIR := $(KDIR)
MDIR := $(realpath $(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
ODIR := $(MDIR)/out/$(VER)

DEPS := Type_info KernCall HooKern
include $(MDIR)/mk/deps.mk

# KallRecon and Type_info ship the same slide.o twice, keep Type_info's copy only
# (KallRecon core.o pulls slide_init/advance/buf from it)
DEPS_OBJS_ALL := $(filter-out deps/KallRecon/lib/slide.o,$(DEPS_OBJS_ALL))

# Type_info: keep port.o (ti_safe_read) and slide.o only, its anchor.o exports
# ti_anchor_set_modname which collides with the same symbol on the device
DEPS_OBJS_ALL := $(filter-out deps/Type_info/lib/btf.o \
	deps/Type_info/lib/query.o deps/Type_info/lib/reg.o deps/Type_info/lib/lib.o \
	deps/Type_info/lib/anchor.o deps/Type_info/lib/dwarf.o,$(DEPS_OBJS_ALL))

# optional HooKern features we do not use
DEPS_OBJS_ALL := $(filter-out deps/HooKern/lib/hk_binder.o deps/HooKern/lib/hk_lsm.o,$(DEPS_OBJS_ALL))

# Compile a generated overlay, keeping the fetched HooKern tree unchanged.
HK_INLINE_SOURCE := $(firstword $(wildcard $(MDIR)/deps/HooKern/lib/hk_inline.c $(SDK_ROOT)/builtin/HooKern/lib/hk_inline.c))
DEPS_OBJS_ALL := $(filter-out deps/HooKern/lib/hk_inline.o .sdk/builtin/HooKern/lib/hk_inline.o,$(DEPS_OBJS_ALL)) src/core/ds_inline.o

# second ko in the same Kbuild: device quirk fixups, independent of droid_lkm
#   ghost task fallback for find_task_by_vpid
#   selftest probe, TEST=1 builds only
# the two optional modules are built only for the targets they compile on.
# the main module is the one being ported, so probe the target kernel for a
# feature that only the newer GKI branches carry instead of naming a version.
DLKM_OPT := $(shell grep -q ' nop_mnt_idmap$$' $(KDIR)/System.map 2>/dev/null && echo m)
obj-$(DLKM_OPT) += droid_lkm_compat.o
DLC_COMPAT_OBJS := compat/compat_main.o compat/ghost.o
ifeq ($(TEST),1)
ccflags-y += -DCONFIG_DROID_LKM_SELFTEST
DLC_COMPAT_OBJS += compat/selftest.o
endif
droid_lkm_compat-y := $(DLC_COMPAT_OBJS) $(DEPS_OBJS_ALL)

# third ko in the same Kbuild: kernel features the device lacks, container scoped
#   xt matches and targets through the exported registration API
#   devtmpfs and root-only identity user namespace compatibility for 6.12
ifeq ($(VERSION).$(PATCHLEVEL),6.12)
obj-$(DLKM_OPT) += droid_lkm_misc.o
endif
DLM_OBJS := src/misc/misc_main.o \
	src/misc/misc_ksym.o \
	src/misc/misc_scope.o \
	src/misc/misc_proc.o \
	src/misc/xt/xt_reg.o \
	src/misc/xt/xt_addrtype.o \
	src/misc/devtmpfs/devtmpfs.o \
	src/misc/userns/userns_core.o \
	src/misc/userns/userns_map.o \
	src/misc/userns/userns_cred.o \
	src/misc/userns/userns_nsops.o \
	src/misc/userns/userns_setns.o \
	src/misc/userns/userns_owner.o \
	src/misc/userns/userns_proc.o \
	src/misc/userns/userns_captrace.o \
	src/misc/userns/userns_hooks.o
droid_lkm_misc-y := $(DLM_OBJS) $(DEPS_OBJS_ALL)

# the locked unmap entry moved twice, pick the unit by what the target kernel
# carries rather than by a version number
MUNMAP_UNIT := $(shell if grep -qE ' [tT] do_vmi_munmap$$' $(KDIR)/System.map 2>/dev/null; then echo src/ipcns/munmap_vmi.o; \
	elif grep -qE ' [tT] do_mas_munmap$$' $(KDIR)/System.map 2>/dev/null; then echo src/ipcns/munmap_mas.o; \
	else echo src/ipcns/munmap_legacy.o; fi)

droid_lkm-y := src/core/main.o src/core/ds_hook.o \
	$(MUNMAP_UNIT) \
	src/core/ds_ksym.o \
	src/core/ds_caps.o \
	src/slot/ds_slot.o \
	src/pidns/pidns.o \
	src/pidns/ds_nsops.o \
	src/pidns/ds_proc.o \
	src/pidns/ds_status.o \
	src/ipcns/ipcns.o \
	src/ipcns/ksym_shim.o \
	src/ipcns/ipc_shim.o \
	src/ipcns/ipc_util.o \
	src/ipcns/ipc_syscall.o \
	src/ipcns/sysv/ipc_msg.o \
	src/ipcns/sysv/ipc_sem.o \
	src/ipcns/sysv/ipc_shm.o \
	src/ipcns/sysv/ipc_msgutil.o \
	src/ipcns/mqueue/ipc_mqueue.o \
	src/ipcns/mqueue/ipc_mqueue_shim.o \
	src/ipcns/mqueue/ipc_mq_sysctl.o \
	src/ipcns/sysctl/ipc_sysctl.o \
	src/ipcns/compat/ipc_compat32.o \
	$(DEPS_OBJS_ALL)

ccflags-y += -std=gnu11
ccflags-y += -Wno-declaration-after-statement
# dead static hk_build_jump in HooKern
ccflags-y += -Wno-unused-function
ccflags-y += -Wno-strict-prototypes
ccflags-y += -I$(src)/src -I$(src)/compat
ccflags-y += -I$(src)/src/core -I$(src)/src/slot -I$(src)/src/pidns
ccflags-y += -I$(src)/src/ipcns -I$(src)/src/ipcns/sysv
ccflags-y += -I$(src)/src/ipcns/mqueue -I$(src)/src/ipcns/sysctl
ccflags-y += -I$(src)/src/ipcns/compat
ccflags-y += -I$(src)/src/misc -I$(src)/src/misc/xt -I$(src)/src/misc/userns
ccflags-y += -I$(src)/src/misc/devtmpfs
ccflags-y += $(addprefix -I$(src)/,$(DEPS_INCS_ALL))
ccflags-y += -I$(obj)/src/core

# KernCall: enable sys_call_table slot patching
ccflags-y += -DCONFIG_KERNSC_PATCH -DCONFIG_KERNSC_DISCOVER

all:
	@mkdir -p $(sort $(dir $(addprefix $(ODIR)/,$(droid_lkm-y) $(droid_lkm_compat-y) $(droid_lkm_misc-y))))
	make -C $(KDIR) M=$(ODIR) src=$(MDIR) modules
clean:
	make -C $(KDIR) M=$(ODIR) src=$(MDIR) clean

$(obj)/%.o: $(src)/%.c $(recordmcount_source) FORCE
	$(call if_changed_rule,cc_o_c)
	$(call cmd,force_checksrc)

$(obj)/%.o: $(src)/%.S FORCE
	$(call if_changed_rule,as_o_S)

$(obj)/src/core/ds_inline_impl.h: $(HK_INLINE_SOURCE) $(src)/scripts/fix-hookern-inline.py
	python3 $(src)/scripts/fix-hookern-inline.py $(HK_INLINE_SOURCE) $@

$(obj)/src/core/ds_inline.o: $(src)/src/core/ds_inline.c $(obj)/src/core/ds_inline_impl.h FORCE
	$(call if_changed_rule,cc_o_c)
	$(call cmd,force_checksrc)
