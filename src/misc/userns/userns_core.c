// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/cred.h>
#include <linux/spinlock.h>
#include <linux/module.h>
#include <linux/err.h>
#include <linux/refcount.h>
#include <linux/user_namespace.h>

#include "misc.h"
#include "misc_ksym.h"
#include "misc_proc.h"
#include "userns.h"

static LIST_HEAD(droid_lkm_userns_list);
static DEFINE_SPINLOCK(droid_lkm_userns_lock);
static unsigned int droid_lkm_userns_count;
static bool droid_lkm_userns_up;

bool droid_lkm_userns_ready(void)
{
	return smp_load_acquire(&droid_lkm_userns_up);
}
/* The host has no userns put path. Bound permanently retained objects. */
#define DROID_LKM_USERNS_MAX 1024

bool droid_lkm_userns_is_ours(const struct user_namespace *uns)
{
	struct droid_lkm_userns *u;
	bool found = false;
	unsigned long irq_flags;

	if (!uns)
		return false;

	spin_lock_irqsave(&droid_lkm_userns_lock, irq_flags);
	list_for_each_entry(u, &droid_lkm_userns_list, link) {
		if (&u->uns == uns) {
			found = true;
			break;
		}
	}
	spin_unlock_irqrestore(&droid_lkm_userns_lock, irq_flags);

	return found;
}

struct droid_lkm_userns *droid_lkm_userns_of(struct task_struct *task)
{
	struct user_namespace *uns;

	if (!task)
		return NULL;

	rcu_read_lock();
	uns = task == current ? current_cred()->user_ns : __task_cred(task)->user_ns;
	rcu_read_unlock();
	if (!droid_lkm_userns_is_ours(uns))
		return NULL;

	return container_of(uns, struct droid_lkm_userns, uns);
}

/*
 * Root-only identity compatibility object. Work and sysctl state are unused;
 * keyring state must still be valid for CONFIG_KEYS callers.
 */
struct droid_lkm_userns *droid_lkm_userns_create(struct droid_lkm_userns *parent)
{
	struct droid_lkm_userns *u;
	unsigned int inum;
	int ret, i;
	unsigned long irq_flags;

	if (parent && parent->level > 32)
		return ERR_PTR(-ENOSPC);

	u = kzalloc(sizeof(*u), GFP_KERNEL);
	if (!u)
		return ERR_PTR(-ENOMEM);

	ret = droid_lkm_misc_ks.proc_alloc_inum(&inum);
	if (ret) {
		kfree(u);
		return ERR_PTR(ret);
	}

	u->level = parent ? parent->level + 1 : 1;
	u->uns.parent = parent ? &parent->uns : droid_lkm_userns_init_uns;
	u->uns.level = u->level;
	u->uns.owner = current_euid();
	u->uns.group = current_egid();
	u->uns.flags = parent ? READ_ONCE(parent->uns.flags) : USERNS_INIT_FLAGS;
	u->uns.parent_could_setfcap = cap_raised(current_cred()->cap_effective,
						 CAP_SETFCAP);
	u->uns.ns.inum = inum;
	u->uns.ns.ops = droid_lkm_userns_ops();
	refcount_set(&u->uns.ns.count, 1);
	u->uns.ucounts = NULL;
	/*
	 * inc_ucount() walks ns->ucount_max[], which zero would turn into ENOSPC for every
	 * copy_utsname(), copy_mnt_ns() and copy_net_ns(), so mirror create_user_ns(). ucounts stays
	 * NULL, nothing is accounted per user
	 */
	for (i = 0; i < UCOUNT_COUNTS; i++)
		u->uns.ucount_max[i] = INT_MAX;
	for (i = 0; i < UCOUNT_RLIMIT_COUNTS; i++)
		u->uns.rlimit_max[i] = LONG_MAX;
#ifdef CONFIG_KEYS
	INIT_LIST_HEAD(&u->uns.keyring_name_list);
	init_rwsem(&u->uns.keyring_sem);
#endif
	INIT_LIST_HEAD(&u->link);

	/* phase one installs the identity map, the only map a container on this kernel could write */
	droid_lkm_userns_map_identity(u);

	spin_lock_irqsave(&droid_lkm_userns_lock, irq_flags);
	if (droid_lkm_userns_count >= DROID_LKM_USERNS_MAX) {
		spin_unlock_irqrestore(&droid_lkm_userns_lock, irq_flags);
		droid_lkm_misc_ks.proc_free_inum(inum);
		kfree(u);
		return ERR_PTR(-ENOSPC);
	}
	/* Creds, owners, cached proc inodes and nsfs callbacks can all outlive
	 * a task. Without host put_user_ns(), releasing this pin is unsafe. */
	__module_get(THIS_MODULE);
	droid_lkm_userns_count++;
	list_add_tail(&u->link, &droid_lkm_userns_list);
	spin_unlock_irqrestore(&droid_lkm_userns_lock, irq_flags);

	droid_lkm_misc_dbg("userns create %p level=%u inum=%u owner=%u\n", u,
		u->level, inum, __kuid_val(u->uns.owner));

	return u;
}

void droid_lkm_userns_destroy(struct droid_lkm_userns *u)
{
	unsigned long irq_flags;
	if (!u)
		return;

	spin_lock_irqsave(&droid_lkm_userns_lock, irq_flags);
	list_del_init(&u->link);
	droid_lkm_userns_count--;
	spin_unlock_irqrestore(&droid_lkm_userns_lock, irq_flags);

	droid_lkm_misc_ks.proc_free_inum(u->uns.ns.inum);
	kfree(u);
	module_put(THIS_MODULE);
}

int droid_lkm_userns_init(void)
{
	int ret;

	/* Never replace native userns operations on a rebuilt kernel. */
	if (droid_lkm_misc_sym("create_user_ns")) {
		droid_lkm_misc_report("userns", "native", "kernel provides CONFIG_USER_NS");
		return -EOPNOTSUPP;
	}
	droid_lkm_userns_init_uns = (void *)droid_lkm_misc_sym("init_user_ns");
	if (!droid_lkm_userns_init_uns)
		return -ENOENT;
	ret = droid_lkm_userns_nsops_init();
	if (ret) {
		droid_lkm_misc_report("userns", "unsupported", "namespace entry failed");
		return ret;
	}

	ret = droid_lkm_userns_owner_init();
	if (ret) {
		droid_lkm_misc_report("userns", "unsupported", "owner symbols missing");
		droid_lkm_userns_nsops_exit();
		return ret;
	}

	ret = droid_lkm_userns_hooks_init();
	if (ret) {
		droid_lkm_misc_report("userns", "unsupported", "a hook failed");
		droid_lkm_userns_owner_exit();
		droid_lkm_userns_nsops_exit();
		return ret;
	}

	droid_lkm_userns_map_selftest();

	ret = droid_lkm_userns_captrace_init();
	if (ret)
		droid_lkm_misc_warn("cap trace unavailable: %d\n", ret);

	ret = droid_lkm_userns_proc_init();
	if (ret) {
		droid_lkm_userns_captrace_exit();
		droid_lkm_userns_hooks_exit();
		droid_lkm_userns_owner_exit();
		droid_lkm_userns_nsops_exit();
		return ret;
	}

	/* Before this point hooks must delegate and proc/nsfs must not publish
	 * callbacks or credentials: a failed module init cannot retain its code. */
	smp_store_release(&droid_lkm_userns_up, true);
	droid_lkm_misc_report("userns", "ready",
			      "root-only identity compatibility; not full isolation");
	droid_lkm_misc_report("userns cap trace", droid_lkm_userns_captrace_on ?
			      "on" : "off", "captrace=<bool>");
	droid_lkm_misc_info("userns ready\n");

	return 0;
}

unsigned int droid_lkm_userns_live(void)
{
	return READ_ONCE(droid_lkm_userns_count);
}

void droid_lkm_userns_exit(void)
{
	smp_store_release(&droid_lkm_userns_up, false);
	/* Successful creation pins the module permanently. This path is only
	 * reachable before any namespace has been published. */
	droid_lkm_userns_proc_exit();
	droid_lkm_userns_captrace_exit();
	droid_lkm_userns_hooks_exit();
	droid_lkm_userns_owner_exit();
	droid_lkm_userns_nsops_exit();
}
