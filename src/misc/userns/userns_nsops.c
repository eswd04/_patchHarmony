// SPDX-License-Identifier: GPL-2.0-only
/*
 * nsfs side of the fake user namespace: the /proc/<pid>/ns/user entry, with a
 * placeholder for tasks that stay in the initial namespace. The kind is
 * registered with the main module, which owns the ns directory lookup.
 */

#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/cred.h>
#include <linux/err.h>
#include <linux/nsproxy.h>
#include <linux/proc_ns.h>
#include <linux/user_namespace.h>
#include <linux/fs_struct.h>
#include <linux/module.h>
#include <linux/capability.h>
#include <linux/fs.h>
#include <uapi/linux/nsfs.h>

#include "misc.h"
#include "misc_ksym.h"
#include "userns.h"
#include "ds_proc.h"
#include "hk_inline.h"

static struct ns_common *droid_lkm_userns_host_placeholder;
static struct proc_ns_operations *droid_lkm_userns_host_ops;
static struct user_namespace *droid_lkm_userns_host_object;
static bool droid_lkm_userns_host_pinned;
static void *(*droid_lkm_userns_symbol_get)(const char *);
static void (*droid_lkm_userns_symbol_put)(const char *);
static bool droid_lkm_userns_main_pinned;
static struct hk_inline droid_lkm_userns_ioctl_hook;
typedef long (*droid_lkm_userns_ioctl_orig_t)(struct file *, unsigned int, unsigned long);
#define droid_lkm_userns_ioctl_orig ((droid_lkm_userns_ioctl_orig_t)READ_ONCE(droid_lkm_userns_ioctl_hook.orig))
static int (*droid_lkm_userns_open_related)(struct ns_common *,
		struct ns_common *(*)(struct ns_common *));

static struct ns_common *droid_lkm_userns_host_pin(void)
{
	if (!xchg(&droid_lkm_userns_host_pinned, true))
		__module_get(THIS_MODULE);
	return droid_lkm_userns_host_placeholder;
}

static struct ns_common *droid_lkm_userns_get(struct task_struct *task)
{
	struct droid_lkm_userns *u = droid_lkm_userns_of(task);

	if (u)
		return &u->uns.ns;

	return droid_lkm_userns_host_pin();
}

static void droid_lkm_userns_put(struct ns_common *ns)
{
	/* Published namespaces and their module are retained until reboot. */
}

static int droid_lkm_userns_install(struct nsset *nsset, struct ns_common *ns)
{
	struct user_namespace *target = container_of(ns, struct user_namespace, ns);
	struct user_namespace *walk = target;
	struct user_namespace *caller = current_cred()->user_ns;
	struct cred *cred = nsset_cred(nsset);

	if (!cred || !droid_lkm_userns_is_ours(target) || target == caller ||
	    !thread_group_empty(current) || !current->fs || current->fs->users != 1)
		return -EINVAL;
	/* Never enter an ancestor or sibling, even with a retained fd. */
	while (walk && walk != caller)
		walk = walk->parent;
	if (!walk || !ns_capable(target, CAP_SYS_ADMIN))
		return -EPERM;
	droid_lkm_userns_install_cred(cred,
		container_of(target, struct droid_lkm_userns, uns));
	return 0;
}

static struct user_namespace *droid_lkm_userns_owner(struct ns_common *ns)
{
	return container_of(ns, struct user_namespace, ns)->parent;
}

static struct ns_common *droid_lkm_userns_get_parent(struct ns_common *ns)
{
	struct user_namespace *uns = container_of(ns, struct user_namespace, ns);
	struct user_namespace *walk = uns->parent;
	struct user_namespace *caller = current_cred()->user_ns;

	while (walk && walk != caller)
		walk = walk->parent;
	if (walk && uns->parent) {
		if (uns->parent == droid_lkm_userns_init_uns)
			return droid_lkm_userns_host_pin();
		return &uns->parent->ns;
	}

	return ERR_PTR(-EPERM);
}

static struct ns_common *droid_lkm_userns_get_owner_ns(struct ns_common *ns)
{
	struct user_namespace *owner, *walk;
	struct user_namespace *caller = current_cred()->user_ns;

	if (!ns->ops->owner)
		return ERR_PTR(-EPERM);
	owner = ns->ops->owner(ns);
	for (walk = owner; walk && walk != caller; walk = walk->parent)
		;
	if (!walk)
		return ERR_PTR(-EPERM);
	if (owner == droid_lkm_userns_init_uns)
		return droid_lkm_userns_host_pin();
	if (!droid_lkm_userns_is_ours(owner))
		return ERR_PTR(-EPERM);
	return &owner->ns;
}

/* ns_get_owner() itself is an inline EPERM stub on the stock build. */
__nocfi noinline long droid_lkm_userns_ioctl_wrap(struct file *file,
		unsigned int cmd, unsigned long arg)
{
	if (droid_lkm_userns_ready() && cmd == NS_GET_USERNS)
		return droid_lkm_userns_open_related(get_proc_ns(file_inode(file)),
						   droid_lkm_userns_get_owner_ns);
	return droid_lkm_userns_ioctl_orig(file, cmd, arg);
}

static const struct proc_ns_operations droid_lkm_userns_ops_real = {
	.name = "user",
	.type = CLONE_NEWUSER,
	.get = droid_lkm_userns_get,
	.put = droid_lkm_userns_put,
	.install = droid_lkm_userns_install,
	.owner = droid_lkm_userns_owner,
	.get_parent = droid_lkm_userns_get_parent,
};

static struct ns_common *droid_lkm_userns_host_get(struct task_struct *task)
{
	return droid_lkm_userns_host_pin();
}

static void droid_lkm_userns_host_put(struct ns_common *ns)
{
}

static int droid_lkm_userns_host_install(struct nsset *nsset, struct ns_common *ns)
{
	return -EINVAL;
}

static struct user_namespace *droid_lkm_userns_host_owner(struct ns_common *ns)
{
	/* the initial namespace has no parent, NS_GET_USERNS reports EPERM */
	return NULL;
}

static struct ns_common *droid_lkm_userns_host_get_parent(struct ns_common *ns)
{
	return ERR_PTR(-EPERM);
}

const struct proc_ns_operations *droid_lkm_userns_ops(void)
{
	return &droid_lkm_userns_ops_real;
}

const struct proc_ns_operations *droid_lkm_userns_ops_for(struct task_struct *task)
{
	if (!droid_lkm_userns_ready())
		return NULL;
	/* The proc symlink inode caches this operations pointer. */
	droid_lkm_userns_host_pin();
	if (task && droid_lkm_userns_of(task))
		return &droid_lkm_userns_ops_real;

	return droid_lkm_userns_host_ops;
}

static const struct droid_lkm_ns_kind_reg droid_lkm_userns_kind = {
	.name = "user",
	.ops_for = droid_lkm_userns_ops_for,
};

int droid_lkm_userns_nsops_init(void)
{
	struct ns_common *ns;
	int ret;

	droid_lkm_userns_host_ops = kzalloc(sizeof(*droid_lkm_userns_host_ops),
					    GFP_KERNEL);
	droid_lkm_userns_host_object = kzalloc(sizeof(*droid_lkm_userns_host_object), GFP_KERNEL);
	ns = droid_lkm_userns_host_object ? &droid_lkm_userns_host_object->ns : NULL;
	if (!droid_lkm_userns_host_ops || !ns) {
		kfree(droid_lkm_userns_host_ops);
		droid_lkm_userns_host_ops = NULL;
		kfree(droid_lkm_userns_host_object);
		droid_lkm_userns_host_object = NULL;
		return -ENOMEM;
	}

	droid_lkm_userns_host_ops->name = "user";
	droid_lkm_userns_host_ops->type = CLONE_NEWUSER;
	droid_lkm_userns_host_ops->get = droid_lkm_userns_host_get;
	droid_lkm_userns_host_ops->put = droid_lkm_userns_host_put;
	droid_lkm_userns_host_ops->install = droid_lkm_userns_host_install;
	droid_lkm_userns_host_ops->owner = droid_lkm_userns_host_owner;
	droid_lkm_userns_host_ops->get_parent = droid_lkm_userns_host_get_parent;

	droid_lkm_userns_host_object->owner = GLOBAL_ROOT_UID;
	droid_lkm_userns_host_object->group = GLOBAL_ROOT_GID;
	ns->inum = PROC_USER_INIT_INO;
	ns->ops = droid_lkm_userns_host_ops;
	refcount_set(&ns->count, 1);
	droid_lkm_userns_host_placeholder = ns;

	droid_lkm_userns_symbol_get = (void *)droid_lkm_misc_sym("__symbol_get");
	droid_lkm_userns_symbol_put = (void *)droid_lkm_misc_sym("__symbol_put");
	if (!droid_lkm_userns_symbol_get || !droid_lkm_userns_symbol_put) {
		droid_lkm_userns_nsops_exit();
		return -ENOENT;
	}
	droid_lkm_misc_ks.ns_kind_register = droid_lkm_userns_symbol_get("droid_lkm_ns_kind_register");
	droid_lkm_misc_ks.ns_kind_unregister = droid_lkm_userns_symbol_get("droid_lkm_ns_kind_unregister");
	if (!droid_lkm_misc_ks.ns_kind_register || !droid_lkm_misc_ks.ns_kind_unregister) {
		if (droid_lkm_misc_ks.ns_kind_register)
			droid_lkm_userns_symbol_put("droid_lkm_ns_kind_register");
		if (droid_lkm_misc_ks.ns_kind_unregister)
			droid_lkm_userns_symbol_put("droid_lkm_ns_kind_unregister");
		droid_lkm_misc_ks.ns_kind_register = NULL;
		droid_lkm_misc_ks.ns_kind_unregister = NULL;
		droid_lkm_userns_nsops_exit();
		return -ENODEV;
	}
	droid_lkm_userns_main_pinned = true;
	droid_lkm_userns_open_related = (void *)droid_lkm_misc_sym("open_related_ns");
	if (!droid_lkm_userns_open_related) {
		droid_lkm_userns_nsops_exit();
		return -ENOENT;
	}
	ret = hk_inline_hook(&droid_lkm_userns_ioctl_hook, "ns_ioctl",
			     "droid_lkm_userns_ioctl_wrap");
	if (ret) {
		droid_lkm_userns_nsops_exit();
		return ret;
	}
	ret = droid_lkm_misc_ks.ns_kind_register(&droid_lkm_userns_kind);
	if (ret)
		droid_lkm_userns_nsops_exit();
	return ret;
}

void droid_lkm_userns_nsops_exit(void)
{
	if (droid_lkm_userns_ioctl_orig) {
		hk_inline_unhook(&droid_lkm_userns_ioctl_hook);
	}
	if (droid_lkm_userns_main_pinned) {
		droid_lkm_misc_ks.ns_kind_unregister(&droid_lkm_userns_kind);
		droid_lkm_userns_symbol_put("droid_lkm_ns_kind_register");
		droid_lkm_userns_symbol_put("droid_lkm_ns_kind_unregister");
		droid_lkm_userns_main_pinned = false;
	}
	kfree(droid_lkm_userns_host_ops);
	kfree(droid_lkm_userns_host_object);
	droid_lkm_userns_host_ops = NULL;
	droid_lkm_userns_host_object = NULL;
	droid_lkm_userns_host_placeholder = NULL;
}
