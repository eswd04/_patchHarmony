#!/usr/bin/env python3
"""Exercise production compat initialization with failing hook/probe stubs."""
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(path, signature):
    source = (ROOT / path).read_text()
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"


stubs = r"""
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#define __init
#define __nocfi
#define noinline
#define unlikely(value) (value)
#define likely(value) (value)
#define READ_ONCE(value) (value)
#define _RET_IP_ 123UL
#define DLC_GHOST_LOG_MAX 3
#define DLC_GHOST_TASK_COMM "ghost-task-sentinel"
static int ready_messages, hook_calls, engine_exit, exit_block, ghost_exit;
static int probe_result, hook_result, build_result, engine_result;
static bool symbols_present = true;
static void log_info(const char *fmt, ...) {
    if (strstr(fmt, "[droid_lkm_compat] ready")) ready_messages++;
}
#define pr_info(...) log_info(__VA_ARGS__)
#define pr_err(...) do {} while (0)
#define pr_warn_once(...) do {} while (0)
enum { HK_INLINE_PLAIN, HK_INLINE_BRANCHED, HK_INLINE_HOOKED };
struct hk_inline { unsigned long orig; };
struct hk_inline_probe { int state; const char *reason; unsigned long target; };
struct module { const char *name; };
struct task_struct { int unused; };
static struct hk_inline dlc_ftbv_hook;
static struct module *(*dlc_module_address)(unsigned long);
extern struct module *__module_address(unsigned long);
extern struct task_struct *find_task_by_vpid(int);
static struct task_struct dlc_ghost_task, real_task;
static unsigned int dlc_ghost_hits;
static bool return_real_task;
static struct task_struct *original(int pid) {
    (void)pid; return return_real_task ? &real_task : NULL;
}
static struct module *module_at(unsigned long addr) {
    (void)addr;
    static struct module module = {"oplus_test"};
    return &module;
}
static bool dlc_ghost_match_module(const char *name) { return !strcmp(name, "oplus_test"); }
static void preempt_disable(void) {}
static void preempt_enable(void) {}
static bool dlc_ghost_ready, dlc_ghost_enable = true;
static bool dlc_inline_hooks_on, dlc_inline_hook = true;
static const char *dlc_ghost_match = "oplus_";
static unsigned int klnum_val = 1;
static unsigned long kallrecon_klp = 1;
static int dlc_hk_cfg;
static int probe_state = HK_INLINE_PLAIN;
static void find_kallsyms_base(void) {}
static int hk_init(const void *cfg) { (void)cfg; return engine_result; }
static void hk_exit(void) { engine_exit++; }
static void hk_exit_block(void) { exit_block++; }
static void dlc_ghost_exit(void) { ghost_exit++; dlc_ghost_ready = false; }
static int dlc_ghost_build(void) {
    if (!build_result) dlc_ghost_ready = true;
    return build_result;
}
static void dlc_ghost_audit_pid_max(void) {}
static unsigned long dlc_ghost_sym(const char *name) {
    (void)name; return symbols_present ? 1 : 0;
}
static int hk_inline_probe(const char *name, struct hk_inline_probe *probe) {
    (void)name; probe->state = probe_state; return probe_result;
}
static int hk_inline_hook(struct hk_inline *hook, const char *sym, const char *wrap) {
    (void)sym; (void)wrap; hook_calls++;
    if (!hook_result) hook->orig = 1;
    return hook_result;
}
"""

production = function("compat/ghost.c", "static inline int dlc_do_inline_hook(")
production += function("compat/ghost.c", "__nocfi noinline struct task_struct *dlc_ftbv_wrap(")
production += function("compat/ghost.c", "int dlc_ghost_init(void)")
production += function("compat/compat_main.c", "static int __init droid_lkm_compat_init(void)")

checks = r"""
static void reset(void) {
    ready_messages = hook_calls = engine_exit = exit_block = ghost_exit = 0;
    probe_result = hook_result = build_result = engine_result = 0;
    probe_state = HK_INLINE_PLAIN;
    dlc_ghost_enable = dlc_inline_hook = symbols_present = true;
    dlc_inline_hooks_on = dlc_ghost_ready = false;
    dlc_ftbv_hook.orig = 0;
    klnum_val = kallrecon_klp = 1;
}
static void failed(int expected) {
    assert(droid_lkm_compat_init() == expected);
    assert(ready_messages == 0 && !dlc_ghost_ready);
    assert(engine_exit == 1 && exit_block == 1 && ghost_exit == 1);
}
int main(void) {
    reset();
    assert(droid_lkm_compat_init() == 0);
    assert(dlc_inline_hooks_on && dlc_ghost_ready);
    assert(hook_calls == 1 && ready_messages == 1 && engine_exit == 0);
    reset(); dlc_inline_hook = false;
    failed(-EOPNOTSUPP); assert(hook_calls == 0);
    reset(); probe_result = -EFAULT;
    failed(-EFAULT); assert(hook_calls == 0);
    reset(); probe_state = HK_INLINE_BRANCHED;
    failed(-EOPNOTSUPP); assert(hook_calls == 0);
    reset(); probe_state = HK_INLINE_HOOKED;
    failed(-EOPNOTSUPP); assert(hook_calls == 0);
    reset(); hook_result = -ENOMEM;
    failed(-ENOMEM); assert(hook_calls == 1);
    reset(); build_result = -ENOENT;
    failed(-ENOENT); assert(hook_calls == 0);
    reset(); symbols_present = false;
    failed(-ENOENT); assert(hook_calls == 0);
    reset(); dlc_inline_hook = false; dlc_ghost_enable = false;
    assert(droid_lkm_compat_init() == 0);
    assert(!dlc_ghost_ready && hook_calls == 0 && ready_messages == 1);
    reset(); engine_result = -EINVAL;
    assert(droid_lkm_compat_init() == -EINVAL);
    assert(hook_calls == 0 && ready_messages == 0 && engine_exit == 0);
    reset(); klnum_val = 0;
    assert(droid_lkm_compat_init() == -ENODATA);
    assert(hook_calls == 0 && ready_messages == 0);
    reset();
    /* Reproduce the moment after the entry is patched, before hook returns. */
    dlc_ftbv_hook.orig = (unsigned long)original;
    dlc_module_address = module_at;
    dlc_ghost_ready = true;
    assert(dlc_ftbv_wrap(123) == &dlc_ghost_task);
    return_real_task = true;
    assert(dlc_ftbv_wrap(123) == &real_task);
    puts("PASS: production compat policy, probe failures and initialization rollback");
}
"""

with tempfile.TemporaryDirectory(prefix="dlkm-compat-unit-") as directory:
    source = Path(directory) / "unit.c"
    binary = Path(directory) / "unit"
    source.write_text(stubs + production + checks)
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra",
                    "-Werror", str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
