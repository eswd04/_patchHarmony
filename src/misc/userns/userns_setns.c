// SPDX-License-Identifier: GPL-2.0-only
/* The host commit_nsset() omits commit_creds() with CONFIG_USER_NS=n.
 * Handle a user nsfs FD directly; delegate every other namespace to the host.
 * pidfd multi-namespace joins are deliberately unsupported on this build. */
#include <linux/cred.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/magic.h>
#include <linux/nsproxy.h>
#include <linux/ptrace.h>

#include "misc.h"
#include "misc_ksym.h"
#include "userns.h"
#include "hk_inline.h"

static struct hk_inline setns_hook;
typedef long (*setns_orig_t)(const struct pt_regs *regs);
#define setns_orig ((setns_orig_t)READ_ONCE(setns_hook.orig))
static struct file *(*userns_fget)(unsigned int fd);
static void (*userns_fput)(struct file *file);
static void (*userns_abort)(struct cred *cred);

__nocfi noinline long droid_lkm_userns_setns_wrap(const struct pt_regs *regs)
{
	struct file *file;
	struct ns_common *ns;
	struct nsset set = { .flags = CLONE_NEWUSER };
	struct cred *new;
	unsigned int flags = regs->regs[1];
	long ret;

	file = userns_fget((unsigned int)regs->regs[0]);
	if (!file)
		return -EBADF;
	if (file_inode(file)->i_sb->s_magic != NSFS_MAGIC) {
		userns_fput(file);
		if (flags & CLONE_NEWUSER)
			return -EOPNOTSUPP;
		return setns_orig(regs);
	}
	ns = get_proc_ns(file_inode(file));
	if (ns->ops->type != CLONE_NEWUSER) {
		userns_fput(file);
		return setns_orig(regs);
	}
	if (flags && flags != CLONE_NEWUSER) {
		ret = -EINVAL;
		goto out;
	}
	/* Host placeholders refuse installation, and foreign objects never
	 * enter our credential path. */
	if (ns->ops != droid_lkm_userns_ops()) {
		ret = -EINVAL;
		goto out;
	}
	new = droid_lkm_misc_ks.prepare_creds();
	if (!new) {
		ret = -ENOMEM;
		goto out;
	}
	set.cred = new;
	ret = ns->ops->install(&set, ns);
	if (ret)
		userns_abort(new);
	else
		ret = droid_lkm_misc_ks.commit_creds(new);
out:
	userns_fput(file);
	return ret;
}

int droid_lkm_userns_setns_init(void)
{
	int ret;

	userns_fget = (void *)droid_lkm_misc_sym("fget");
	userns_fput = (void *)droid_lkm_misc_sym("fput");
	userns_abort = (void *)droid_lkm_misc_sym("abort_creds");
	if (!userns_fget || !userns_fput || !userns_abort)
		return -ENOENT;
	ret = hk_inline_hook(&setns_hook, "__arm64_sys_setns",
			     "droid_lkm_userns_setns_wrap");
	return ret;
}

void droid_lkm_userns_setns_exit(void)
{
	if (setns_orig) {
		hk_inline_unhook(&setns_hook);
	}
}
