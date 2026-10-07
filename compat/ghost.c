// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#include <linux/module.h>
#include <linux/percpu.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/list.h>
#include <linux/refcount.h>
#include <linux/string.h>
#include <linux/preempt.h>
#include <linux/threads.h>

#include "core.h"
#include "ghost.h"
#include "hk_inline.h"

#define DLC_GHOST_TASK_COMM	"ghost-task-sentinel"
#define DLC_GHOST_LOG_MAX	3

static bool dlc_ghost_enable = true;
module_param_named(ghost, dlc_ghost_enable, bool, 0444);
MODULE_PARM_DESC(ghost, "hand out a ghost task when find_task_by_vpid misses, default 1");

static char *dlc_ghost_match = "oplus_";
module_param_named(ghost_match, dlc_ghost_match, charp, 0444);
MODULE_PARM_DESC(ghost_match, "caller module name prefix, * matches all, default oplus_");

static typeof(&__module_address) dlc_module_address;
extern bool dlc_inline_hooks_on;

/*
 * The vendor fixup requires this inline hook. Disabling it while ghost=1
 * refuses module initialization; ghost=0 explicitly requests no vendor fixup.
 */
static inline int dlc_do_inline_hook(struct hk_inline *h, const char *sym,
				  const char *wrap)
{
	if (!dlc_inline_hooks_on) {
		pr_warn_once("[droid_lkm_compat] inline hooks are off (inline_hook=0), %s not hooked\n", sym);
		return -EOPNOTSUPP;
	}
	return hk_inline_hook(h, sym, wrap);
}

static struct hk_inline dlc_ftbv_hook;
static struct task_struct dlc_ghost_task;
static bool dlc_ghost_ready;
static unsigned int dlc_ghost_hits;

static unsigned long dlc_ghost_sym(const char *name)
{
	if (!kallrecon_klp)
		return 0;
	return kallrecon_klp(name);
}

static bool dlc_ghost_match_module(const char *name)
{
	size_t n;

	if (!dlc_ghost_match || !*dlc_ghost_match)
		return false;
	if (dlc_ghost_match[0] == '*' && !dlc_ghost_match[1])
		return true;
	n = strlen(dlc_ghost_match);
	return strncmp(name, dlc_ghost_match, n) == 0;
}

// _RET_IP_ must be read before calling orig
__nocfi noinline struct task_struct *dlc_ftbv_wrap(pid_t vnr)
{
	unsigned long ret_ip = (unsigned long)_RET_IP_;
	struct task_struct *task;
	typeof(&find_task_by_vpid) orig;
	struct module *mod;
	bool match = false;

	/* HooKern publishes h->orig before patching the live entry. A separate
	 * pointer assigned after hk_inline_hook() returns leaves a window in
	 * which vendor callers receive NULL even though the hook is installed.
	 */
	orig = (typeof(orig))READ_ONCE(dlc_ftbv_hook.orig);
	if (unlikely(!orig))
		return NULL;

	task = orig(vnr);
	if (likely(task))
		return task;

	if (!dlc_ghost_ready)
		return task;

	if (dlc_module_address) {
		preempt_disable();
		mod = dlc_module_address(ret_ip);
		if (mod)
			match = dlc_ghost_match_module(mod->name);
		preempt_enable();
		if (!match)
			return task;
		dlc_ghost_hits++;
		if (dlc_ghost_hits <= DLC_GHOST_LOG_MAX)
			pr_info("[droid_lkm_compat] ghost: pid=%d caller=%pS -> %s\n",
				(int)vnr, (void *)ret_ip, DLC_GHOST_TASK_COMM);
		return &dlc_ghost_task;
	}

	return task;
}

// ghost is a shallow copy of init_task, list heads re-init to self
static int dlc_ghost_build(void)
{
	unsigned long init_task_addr = dlc_ghost_sym("init_task");

	if (!init_task_addr)
		return -ENOENT;

	memcpy(&dlc_ghost_task, (void *)init_task_addr, sizeof(struct task_struct));
	INIT_LIST_HEAD(&dlc_ghost_task.tasks);
	INIT_LIST_HEAD(&dlc_ghost_task.children);
	INIT_LIST_HEAD(&dlc_ghost_task.sibling);
	INIT_LIST_HEAD(&dlc_ghost_task.thread_node);
	INIT_LIST_HEAD(&dlc_ghost_task.ptraced);
	INIT_LIST_HEAD(&dlc_ghost_task.ptrace_entry);
	strscpy(dlc_ghost_task.comm, DLC_GHOST_TASK_COMM, TASK_COMM_LEN);
	/*
	 * vendors index PID_MAX_DEFAULT sized arrays by p->pid, so the pid has
	 * to stay inside that range; init_task.thread_pid is init_struct_pid
	 * (nr 0) so task_pid_vnr()/task_tgid_vnr() report the same 0 and stay
	 * consistent with the raw fields. slot 0 belongs to swapper, no task in
	 * the pid array is ever reached through it.
	 */
	dlc_ghost_task.pid = 0;
	dlc_ghost_task.tgid = 0;
	refcount_set(&dlc_ghost_task.usage, 1000);
	dlc_ghost_ready = true;
	return 0;
}

/*
 * the ghost task only covers lookups that miss; a pid_max above
 * PID_MAX_DEFAULT lets real tasks carry pids the same vendors cannot index
 */
static void dlc_ghost_audit_pid_max(void)
{
	unsigned long addr = dlc_ghost_sym("pid_max");
	int value;

	if (!addr)
		return;
	value = *(int *)addr;
	if (value > PID_MAX_DEFAULT)
		pr_warn("[droid_lkm_compat] ghost: pid_max=%d exceeds %d\n",
			value, PID_MAX_DEFAULT);
}

void dlc_ghost_exit(void)
{
	if (dlc_ftbv_hook.orig)
		hk_inline_unhook(&dlc_ftbv_hook);
	dlc_ghost_ready = false;
}

int dlc_ghost_init(void)
{
	int ret;

	if (!dlc_ghost_enable)
		return 0;
	if (!dlc_inline_hooks_on) {
		pr_err("[droid_lkm_compat] ghost=1 requires inline_hook=1\n");
		return -EOPNOTSUPP;
	}

	ret = dlc_ghost_build();
	if (ret) {
		pr_err("[droid_lkm_compat] ghost: init_task not found\n");
		return ret;
	}
	dlc_ghost_audit_pid_max();

	dlc_module_address = (typeof(dlc_module_address))dlc_ghost_sym("__module_address");
	if (!dlc_module_address) {
		pr_err("[droid_lkm_compat] ghost: __module_address not found\n");
		dlc_ghost_ready = false;
		return -ENOENT;
	}

	/*
	 * the engine patches the end of a branch chain when the entry is a thunk,
	 * and on a +lto kernel find_task_by_vpid is one. the trampoline of that
	 * build loops back into the patched entry, so a call recurses until the
	 * kernel stack is gone. judge the entry first and refuse a shape the
	 * engine cannot drive, the feature degrades instead of panicking
	 */
	{
		struct hk_inline_probe probe;

		memset(&probe, 0, sizeof(probe));
		ret = hk_inline_probe("find_task_by_vpid", &probe);
		if (ret) {
			pr_err("[droid_lkm_compat] ghost: entry probe failed %d\n", ret);
			dlc_ghost_ready = false;
			return ret;
		}
		if (probe.state != HK_INLINE_PLAIN) {
			pr_err("[droid_lkm_compat] ghost: find_task_by_vpid entry state %d (%s) target 0x%lx, inline hook refused\n",
			       probe.state, probe.reason ? probe.reason : "?",
			       probe.target);
			dlc_ghost_ready = false;
			return -EOPNOTSUPP;
		}
	}

	ret = dlc_do_inline_hook(&dlc_ftbv_hook, "find_task_by_vpid", "dlc_ftbv_wrap");
	if (ret) {
		pr_err("[droid_lkm_compat] ghost: hook find_task_by_vpid failed %d\n", ret);
		dlc_ghost_ready = false;
		return ret;
	}
	pr_info("[droid_lkm_compat] ghost: find_task_by_vpid hooked, match=%s\n",
		dlc_ghost_match ? dlc_ghost_match : "");
	return 0;
}
