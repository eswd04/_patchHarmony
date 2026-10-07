// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 *
 * The chokepoints that cannot be answered by correcting state.
 *
 * ksys_unshare and copy_creds are where a request for CLONE_NEWUSER is turned
 * into a fake namespace. The flag is stripped before the kernel sees it, the
 * kernel keeps handling the other namespaces, and the credential side is
 * installed afterwards: on current for unshare, on the child for clone, which
 * cannot have run yet at that point.
 *
 * unshare_nsproxy_namespaces and capable_wrt_inode_uidgid are the two decisions
 * that read current_user_ns(), which this kernel compiles to a constant, so
 * there is no state to correct: the question itself names the host. Both keep
 * the original as the fast path and only add the container case. The owners the
 * kernel fills in through the get_user_ns() stub are corrected in
 * userns_owner.c, which is why may_mount() and mount_capable() have no hook:
 * may_mount() is inlined into path_mount() and could not be hooked anyway.
 */

#include <linux/sched.h>
#include <linux/cred.h>
#include <linux/capability.h>
#include <linux/nsproxy.h>
#include <linux/utsname.h>
#include <linux/cgroup.h>
#include <linux/time_namespace.h>
#include <net/net_namespace.h>
#include <linux/fs.h>
#include <linux/fs_context.h>
#include <linux/mnt_idmapping.h>
#include <linux/err.h>
#include <linux/uidgid.h>
#include <linux/fs_struct.h>
#include <linux/ptrace.h>
#include <linux/prctl.h>

#include "misc.h"
#include "misc_ksym.h"
#include "userns.h"
#include "hk_inline.h"

static struct hk_inline droid_lkm_userns_unshare_hook;
static struct hk_inline droid_lkm_userns_nsproxy_hook;
static struct hk_inline droid_lkm_userns_creds_hook;
static struct hk_inline droid_lkm_userns_inodecap_hook;
static struct hk_inline droid_lkm_userns_setid_hook;
static struct hk_inline droid_lkm_userns_groups_hook;
static struct hk_inline droid_lkm_userns_groups16_hook;
static struct hk_inline droid_lkm_userns_prctl_hook;
typedef long (*droid_lkm_userns_groups16_orig_t)(const struct pt_regs *);
#define droid_lkm_userns_groups16_orig ((droid_lkm_userns_groups16_orig_t)READ_ONCE(droid_lkm_userns_groups16_hook.orig))
typedef int (*droid_lkm_userns_prctl_orig_t)(int, unsigned long, unsigned long,
				       unsigned long, unsigned long);
#define droid_lkm_userns_prctl_orig ((droid_lkm_userns_prctl_orig_t)READ_ONCE(droid_lkm_userns_prctl_hook.orig))
typedef bool (*droid_lkm_userns_setid_orig_t)(struct user_namespace *, int);
#define droid_lkm_userns_setid_orig ((droid_lkm_userns_setid_orig_t)READ_ONCE(droid_lkm_userns_setid_hook.orig))
typedef long (*droid_lkm_userns_groups_orig_t)(const struct pt_regs *);
#define droid_lkm_userns_groups_orig ((droid_lkm_userns_groups_orig_t)READ_ONCE(droid_lkm_userns_groups_hook.orig))

typedef int (*droid_lkm_userns_unshare_orig_t)(unsigned long flags);
#define droid_lkm_userns_unshare_orig ((droid_lkm_userns_unshare_orig_t)READ_ONCE(droid_lkm_userns_unshare_hook.orig))
typedef int (*droid_lkm_userns_nsproxy_orig_t)(unsigned long unshare_flags,
					    struct nsproxy **new_nsp,
					    struct cred *new_cred,
					    struct fs_struct *new_fs);
#define droid_lkm_userns_nsproxy_orig ((droid_lkm_userns_nsproxy_orig_t)READ_ONCE(droid_lkm_userns_nsproxy_hook.orig))
typedef int (*droid_lkm_userns_creds_orig_t)(struct task_struct *p,
					  unsigned long clone_flags);
#define droid_lkm_userns_creds_orig ((droid_lkm_userns_creds_orig_t)READ_ONCE(droid_lkm_userns_creds_hook.orig))
typedef bool (*droid_lkm_userns_inodecap_orig_t)(struct mnt_idmap *idmap,
					      const struct inode *inode, int cap);
#define droid_lkm_userns_inodecap_orig ((droid_lkm_userns_inodecap_orig_t)READ_ONCE(droid_lkm_userns_inodecap_hook.orig))

static const struct cred *(*droid_lkm_userns_override)(const struct cred *);
static void (*droid_lkm_userns_revert)(const struct cred *);
static void (*droid_lkm_userns_abort)(struct cred *);
static bool (*droid_lkm_userns_chrooted)(void);
static int (*droid_lkm_userns_security_create)(const struct cred *);

/* An identity map over every host ID is only meaningful for a privileged
 * creator. Allowing an Android app to receive it would grant DAC privileges. */
int droid_lkm_userns_create_check(void)
{
	struct user_namespace *ns = current_cred()->user_ns;

	if (!uid_eq(current_euid(), GLOBAL_ROOT_UID) ||
	    !ns_capable(ns, CAP_SYS_ADMIN) ||
	    !ns_capable(ns, CAP_SETUID) || !ns_capable(ns, CAP_SETGID) ||
	    !ns_capable(ns, CAP_SETFCAP))
		return -EPERM;
	if (ns->level > 32)
		return -ENOSPC;
	if (droid_lkm_userns_chrooted())
		return -EPERM;
	return droid_lkm_userns_security_create(current_cred());
}

/*
 * unshare_nsproxy_namespaces() picks current_user_ns(), the initial namespace here, and the
 * capability test then fails for a task in a fake one. build them with the task's own
 * namespace as owner, through create_new_namespaces() so every constructor runs
 */
__nocfi noinline int droid_lkm_userns_nsproxy_wrap(unsigned long unshare_flags,
						   struct nsproxy **new_nsp,
						   struct cred *new_cred,
						   struct fs_struct *new_fs)
{
	struct droid_lkm_userns *u = droid_lkm_userns_of(current);
	struct nsproxy *nsp;
	int ret;

	if (!(unshare_flags & (CLONE_NEWNS | CLONE_NEWUTS | CLONE_NEWIPC |
		CLONE_NEWNET | CLONE_NEWPID | CLONE_NEWCGROUP | CLONE_NEWTIME)))
		return 0;

	if (!u || new_cred)
		return droid_lkm_userns_nsproxy_orig(unshare_flags, new_nsp, new_cred, new_fs);

	if (!ns_capable(&u->uns, CAP_SYS_ADMIN))
		return -EPERM;

	nsp = droid_lkm_misc_ks.create_new_namespaces(unshare_flags, current, &u->uns,
						      new_fs ? new_fs : current->fs);
	if (IS_ERR(nsp))
		return PTR_ERR(nsp);

	/*
	 * get_user_ns() is a stub returning the initial namespace, so the constructors stored the
	 * wrong owner. only namespaces this call created are touched, the mount namespace is covered
	 * by may_mount()
	 */
	if (unshare_flags & CLONE_NEWUTS)
		nsp->uts_ns->user_ns = &u->uns;
	if (unshare_flags & CLONE_NEWNET)
		nsp->net_ns->user_ns = &u->uns;
	if (unshare_flags & CLONE_NEWCGROUP)
		nsp->cgroup_ns->user_ns = &u->uns;
	if ((unshare_flags & CLONE_NEWTIME) && nsp->time_ns_for_children)
		nsp->time_ns_for_children->user_ns = &u->uns;

	*new_nsp = nsp;
	ret = 0;

	return ret;
}

__nocfi noinline int droid_lkm_userns_unshare_wrap(unsigned long flags)
{
	struct droid_lkm_userns *u;
	struct cred *new;
	const struct cred *saved;
	int ret;

	if (!droid_lkm_userns_ready() || !(flags & CLONE_NEWUSER))
		return droid_lkm_userns_unshare_orig(flags);

	/* NEWUSER implies THREAD and FS for unshare, including explicit FS.
	 * Keep the kernel's flag and single-thread checks intact. */
	if (flags & ~(CLONE_THREAD | CLONE_FS | CLONE_NEWNS | CLONE_SIGHAND |
		CLONE_VM | CLONE_FILES | CLONE_SYSVSEM | CLONE_NEWUTS |
		CLONE_NEWIPC | CLONE_NEWNET | CLONE_NEWUSER | CLONE_NEWPID |
		CLONE_NEWCGROUP | CLONE_NEWTIME))
		return -EINVAL;
	if (!thread_group_empty(current))
		return -EINVAL;
	ret = droid_lkm_userns_create_check();
	if (ret)
		return ret;
	new = droid_lkm_misc_ks.prepare_creds();
	if (!new)
		return -ENOMEM;
	u = droid_lkm_userns_create(droid_lkm_userns_of(current));
	if (IS_ERR(u)) {
		droid_lkm_userns_abort(new);
		return PTR_ERR(u);
	}
	droid_lkm_userns_install_cred(new, u);

	/* Give constructors the staged owner without committing credentials.
	 * The original cannot allocate a new cred after NEWUSER is removed.
	 * Revert before commit_creds(), which requires real_cred == cred. */
	saved = droid_lkm_userns_override(new);
	ret = droid_lkm_userns_unshare_orig((flags & ~CLONE_NEWUSER) |
					   CLONE_THREAD | CLONE_FS);
	droid_lkm_userns_revert(saved);
	if (ret) {
		droid_lkm_userns_abort(new);
		/* A constructor can defer destruction of an object that already
		 * carries this owner. The host has no put_user_ns() callback to
		 * tell us when that reference has gone away. Retain the owner. */
		return ret;
	}
	return droid_lkm_misc_ks.commit_creds(new);
}

__nocfi noinline int droid_lkm_userns_creds_wrap(struct task_struct *p,
						unsigned long clone_flags)
{
	struct droid_lkm_userns *u;
	unsigned long rest;
	int ret;

	/* a thread clone shares credentials, so it cannot own a namespace */
	if (!droid_lkm_userns_ready() || !(clone_flags & CLONE_NEWUSER))
		return droid_lkm_userns_creds_orig(p, clone_flags);

	if (clone_flags & (CLONE_THREAD | CLONE_FS))
		return -EINVAL;
	ret = droid_lkm_userns_create_check();
	if (ret)
		return ret;
	u = droid_lkm_userns_create(droid_lkm_userns_of(current));
	if (IS_ERR(u))
		return PTR_ERR(u);

	rest = clone_flags & ~CLONE_NEWUSER;
	ret = droid_lkm_userns_creds_orig(p, rest);
	if (ret) {
		droid_lkm_userns_destroy(u);
		return ret;
	}

	/* the child has not run and copy_creds() built a fresh credential, so this is race free */
	droid_lkm_userns_install_cred((struct cred *)p->cred, u);

	return 0;
}

__nocfi noinline bool droid_lkm_userns_inodecap_wrap(struct mnt_idmap *idmap,
						     const struct inode *inode,
						     int cap)
{
	struct droid_lkm_userns *u;

	if (droid_lkm_userns_inodecap_orig(idmap, inode, cap))
		return true;

	/* an idmapped mount is a different rule set, it arrives with the lens */
	if (droid_lkm_misc_ks.nop_mnt_idmap && idmap != droid_lkm_misc_ks.nop_mnt_idmap)
		return false;

	u = droid_lkm_userns_of(current);
	if (!u)
		return false;

	if (!ns_capable(&u->uns, cap))
		return false;

	/* privileged_wrt_inode_uidgid(): both ids must be mapped in the namespace */
	return uid_valid(inode->i_uid) && gid_valid(inode->i_gid);
}

/* Only the setid capability API is translated. Ordinary capable() checks
 * still target the host and remain denied for these credentials. */
__nocfi noinline bool droid_lkm_userns_setid_wrap(struct user_namespace *ns, int cap)
{
	struct droid_lkm_userns *u = droid_lkm_userns_of(current);

	if (u && ns == droid_lkm_userns_init_uns &&
	    (cap == CAP_SETUID || cap == CAP_SETGID))
		ns = &u->uns;
	return droid_lkm_userns_setid_orig(ns, cap);
}

/* may_setgroups() can be inlined into setgroups, so enforce deny at the
 * syscall entry as well as translating the out-of-line setid API. */
__nocfi noinline long droid_lkm_userns_groups_wrap(const struct pt_regs *regs)
{
	struct droid_lkm_userns *u = droid_lkm_userns_of(current);

	if (u && !(READ_ONCE(u->uns.flags) & USERNS_SETGROUPS_ALLOWED))
		return -EPERM;
	return droid_lkm_userns_groups_orig(regs);
}

__nocfi noinline long droid_lkm_userns_groups16_wrap(const struct pt_regs *regs)
{
	struct droid_lkm_userns *u = droid_lkm_userns_of(current);

	if (u && !(READ_ONCE(u->uns.flags) & USERNS_SETGROUPS_ALLOWED))
		return -EPERM;
	return droid_lkm_userns_groups16_orig(regs);
}

__nocfi noinline int droid_lkm_userns_prctl_wrap(int option, unsigned long arg2,
		unsigned long arg3, unsigned long arg4, unsigned long arg5)
{
	struct droid_lkm_userns *u = droid_lkm_userns_of(current);
	struct cred *new;

	if (!u || option != PR_CAPBSET_DROP)
		return droid_lkm_userns_prctl_orig(option, arg2, arg3, arg4, arg5);
	/* The host cap_prctl_drop() uses the constant current_user_ns(). */
	if (!ns_capable(&u->uns, CAP_SETPCAP))
		return -EPERM;
	if (!cap_valid(arg2))
		return -EINVAL;
	new = droid_lkm_misc_ks.prepare_creds();
	if (!new)
		return -ENOMEM;
	cap_lower(new->cap_bset, arg2);
	return droid_lkm_misc_ks.commit_creds(new);
}

static int droid_lkm_userns_hook_one(struct hk_inline *h, const char *sym,
				     const char *wrap)
{
	int ret;

	ret = hk_inline_hook(h, sym, wrap);
	if (ret) {
		droid_lkm_misc_warn("hook %s failed: %d\n", sym, ret);
		return ret;
	}

	droid_lkm_misc_dbg("hook %-28s addr=0x%lx orig=0x%lx\n", sym, h->addr, h->orig);

	return 0;
}

int droid_lkm_userns_hooks_init(void)
{
	int ret;

	droid_lkm_userns_override = (void *)droid_lkm_misc_sym("override_creds");
	droid_lkm_userns_revert = (void *)droid_lkm_misc_sym("revert_creds");
	droid_lkm_userns_abort = (void *)droid_lkm_misc_sym("abort_creds");
	droid_lkm_userns_chrooted = (void *)droid_lkm_misc_sym("current_chrooted");
	droid_lkm_userns_security_create = (void *)droid_lkm_misc_sym("security_create_user_ns");
	if (!droid_lkm_userns_override || !droid_lkm_userns_revert ||
	    !droid_lkm_userns_abort || !droid_lkm_userns_chrooted ||
	    !droid_lkm_userns_security_create)
		return -ENOENT;

	ret = droid_lkm_userns_hook_one(&droid_lkm_userns_unshare_hook, "ksys_unshare",
		"droid_lkm_userns_unshare_wrap");
	if (ret)
		return ret;

	ret = droid_lkm_userns_hook_one(&droid_lkm_userns_nsproxy_hook,
		"unshare_nsproxy_namespaces", "droid_lkm_userns_nsproxy_wrap");
	if (ret)
		goto out_unshare;

	ret = droid_lkm_userns_hook_one(&droid_lkm_userns_creds_hook, "copy_creds",
		"droid_lkm_userns_creds_wrap");
	if (ret)
		goto out_nsproxy;

	ret = droid_lkm_userns_hook_one(&droid_lkm_userns_inodecap_hook,
		"capable_wrt_inode_uidgid", "droid_lkm_userns_inodecap_wrap");
	if (ret)
		goto out_creds;

	ret = droid_lkm_userns_hook_one(&droid_lkm_userns_setid_hook,
		"ns_capable_setid", "droid_lkm_userns_setid_wrap");
	if (ret)
		goto out_inodecap;
	ret = droid_lkm_userns_hook_one(&droid_lkm_userns_groups_hook,
		"__arm64_sys_setgroups", "droid_lkm_userns_groups_wrap");
	if (ret)
		goto out_setid;
	if (droid_lkm_misc_sym("__arm64_sys_setgroups16")) {
		ret = droid_lkm_userns_hook_one(&droid_lkm_userns_groups16_hook,
			"__arm64_sys_setgroups16", "droid_lkm_userns_groups16_wrap");
		if (ret)
			goto out_groups;
	}
	ret = droid_lkm_userns_hook_one(&droid_lkm_userns_prctl_hook,
		"cap_task_prctl", "droid_lkm_userns_prctl_wrap");
	if (ret)
		goto out_groups16;
	ret = droid_lkm_userns_setns_init();
	if (ret)
		goto out_prctl;

	droid_lkm_misc_info("userns hooks installed\n");
	return 0;

out_prctl:
	hk_inline_unhook(&droid_lkm_userns_prctl_hook);
out_groups16:
	if (droid_lkm_userns_groups16_orig) {
		hk_inline_unhook(&droid_lkm_userns_groups16_hook);
	}
out_groups:
	hk_inline_unhook(&droid_lkm_userns_groups_hook);
out_setid:
	hk_inline_unhook(&droid_lkm_userns_setid_hook);
out_inodecap:
	hk_inline_unhook(&droid_lkm_userns_inodecap_hook);
out_creds:
	hk_inline_unhook(&droid_lkm_userns_creds_hook);
out_nsproxy:
	hk_inline_unhook(&droid_lkm_userns_nsproxy_hook);
out_unshare:
	hk_inline_unhook(&droid_lkm_userns_unshare_hook);
	return ret;
}

void droid_lkm_userns_hooks_exit(void)
{
	droid_lkm_userns_setns_exit();
	hk_inline_unhook(&droid_lkm_userns_prctl_hook);
	if (droid_lkm_userns_groups16_orig) {
		hk_inline_unhook(&droid_lkm_userns_groups16_hook);
	}
	hk_inline_unhook(&droid_lkm_userns_groups_hook);
	hk_inline_unhook(&droid_lkm_userns_setid_hook);
	hk_inline_unhook(&droid_lkm_userns_inodecap_hook);
	hk_inline_unhook(&droid_lkm_userns_creds_hook);
	hk_inline_unhook(&droid_lkm_userns_nsproxy_hook);
	hk_inline_unhook(&droid_lkm_userns_unshare_hook);
}
