// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2026 dere3046
 */

#ifndef DROID_LKM_MISC_USERNS_H
#define DROID_LKM_MISC_USERNS_H

#include <linux/types.h>
#include <linux/list.h>
#include <linux/uidgid.h>
#include <linux/ns_common.h>
#include <linux/user_namespace.h>
#include <linux/proc_ns.h>

struct task_struct;
struct cred;

/*
 * Root-only compatibility namespace. The host inline UID/GID conversions
 * require an eagerly active full identity map. Writes acknowledge each map
 * once; they cannot install subsets or translations. Published objects and
 * their module are retained because the host has no userns release path.
 */
struct droid_lkm_userns {
	struct user_namespace uns;
	struct list_head link;
	unsigned int level;
	bool uid_written;
	bool gid_written;
};

int droid_lkm_userns_init(void);
void droid_lkm_userns_exit(void);
bool droid_lkm_userns_ready(void);

int droid_lkm_userns_hooks_init(void);
void droid_lkm_userns_hooks_exit(void);

/* the initial user namespace, resolved once and shared by the owner work */
extern struct user_namespace *droid_lkm_userns_init_uns;

int droid_lkm_userns_owner_init(void);
void droid_lkm_userns_owner_exit(void);

extern bool droid_lkm_userns_captrace_on;
int droid_lkm_userns_captrace_init(void);
void droid_lkm_userns_captrace_exit(void);

/* /proc/<pid>/{uid_map,gid_map,setgroups}, added back under the pid directory */
int droid_lkm_userns_proc_init(void);
void droid_lkm_userns_proc_exit(void);

struct droid_lkm_userns *droid_lkm_userns_create(struct droid_lkm_userns *parent);
void droid_lkm_userns_destroy(struct droid_lkm_userns *u);
struct droid_lkm_userns *droid_lkm_userns_of(struct task_struct *task);
unsigned int droid_lkm_userns_live(void);
bool droid_lkm_userns_is_ours(const struct user_namespace *uns);

int droid_lkm_userns_create_check(void);
int droid_lkm_userns_setns_init(void);
void droid_lkm_userns_setns_exit(void);

int droid_lkm_userns_install_current(struct droid_lkm_userns *u);
void droid_lkm_userns_install_cred(struct cred *new, struct droid_lkm_userns *u);

const struct proc_ns_operations *droid_lkm_userns_ops(void);
const struct proc_ns_operations *droid_lkm_userns_ops_for(struct task_struct *task);
int droid_lkm_userns_nsops_init(void);
void droid_lkm_userns_nsops_exit(void);

u32 droid_lkm_userns_map_down(const struct droid_lkm_userns *u, u32 id);
u32 droid_lkm_userns_map_up(const struct droid_lkm_userns *u, u32 id);
bool droid_lkm_userns_map_ok(const struct droid_lkm_userns *u, u32 id);
void droid_lkm_userns_map_identity(struct droid_lkm_userns *u);
void droid_lkm_userns_map_selftest(void);

#endif
