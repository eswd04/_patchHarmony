#!/usr/bin/env python3
"""Compile the actual map helpers and proc identity parser with userspace stubs."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
maps = (root / "src/misc/userns/userns_map.c").read_text()
maps = "\n".join(line for line in maps.splitlines() if not line.startswith("#include"))
proc = (root / "src/misc/userns/userns_proc.c").read_text()
start = proc.index("static bool droid_lkm_userns_map_is_identity(")
end = proc.index("static ssize_t droid_lkm_userns_map_read", start)
parser = proc[start:end]
capability_type = next(line for line in proc.splitlines()
                       if line.startswith("static ") and "droid_lkm_userns_file_capable" in line)

plumbing = (root / "src/misc/misc_proc.c").read_text()
start = plumbing.index("ssize_t droid_lkm_misc_proc_read_text(")
end = plumbing.index("loff_t droid_lkm_misc_proc_llseek", start)
read_text = plumbing[start:end]

owner = (root / "src/misc/userns/userns_owner.c").read_text()
start = owner.index("static bool droid_lkm_userns_is_text(")
end = owner.index("/*", owner.index("\n}", start))
text_address = owner[start:end]

stubs = r"""
#include <assert.h>
#include <errno.h>
#include <sys/types.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define __user
#define copy_to_user(dst, src, n) (memcpy(dst, src, n), 0)
typedef uint32_t u32;
typedef uint64_t u64;
struct file;
struct user_namespace;
extern bool file_ns_capable(const struct file *, struct user_namespace *, int);
#define UID_GID_MAP_MAX_BASE_EXTENTS 5
#define smp_rmb() do {} while (0)
#define droid_lkm_misc_err(...) assert(!"kernel map selftest failed")
#define droid_lkm_misc_info(...) do {} while (0)
#define DROID_LKM_USERNS_TEXT_WIN (64UL << 20)
static unsigned long kernel_base = 0xffffffc080000000UL;
struct uid_gid_extent { u32 first, lower_first, count; };
struct uid_gid_map { struct uid_gid_extent extent[5]; u32 nr_extents; };
struct user_namespace { struct uid_gid_map uid_map, gid_map; };
struct droid_lkm_userns { struct user_namespace uns; };
"""
checks = r"""
int main(void)
{
    _Static_assert(__builtin_types_compatible_p(
        typeof(droid_lkm_userns_file_capable), typeof(&file_ns_capable)),
        "file_ns_capable must retain its exact kernel type for KCFI");
    /* Actual phone mntns_owner is at _text+0x47625c, aligned to 4 but not 8. */
    assert(droid_lkm_userns_is_text(kernel_base + 0x47625c));
    assert(droid_lkm_userns_is_text(kernel_base + 0x476258));
    assert(!droid_lkm_userns_is_text(kernel_base + 0x47625d));
    assert(!droid_lkm_userns_is_text(kernel_base - 4));
    assert(!droid_lkm_userns_is_text(kernel_base + DROID_LKM_USERNS_TEXT_WIN));
    struct droid_lkm_userns ns = {0};
    const char *valid[] = {"0 0 4294967295", " 0\t0 4294967295\n", "00 00 4294967295\n"};
    const char *invalid[] = {"", "0", "0 0", "0 0 0", "0 0 4294967296",
        "0 1000 1", "0 0 1", "0 0 -1", "0 0 4294967295x", "0\n0 4294967295",
        "0 0 4294967295\n1 1 1", "0 0 18446744073709551615"};
    for (unsigned i = 0; i < sizeof(valid) / sizeof(valid[0]); i++)
        assert(droid_lkm_userns_map_is_identity(valid[i]));
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++)
        assert(!droid_lkm_userns_map_is_identity(invalid[i]));
    assert(droid_lkm_userns_map_down(&ns, 0) == UINT32_MAX);
    droid_lkm_userns_map_identity(&ns);
    assert(droid_lkm_userns_map_down(&ns, UINT32_MAX - 1) == UINT32_MAX - 1);
    assert(droid_lkm_userns_map_up(&ns, UINT32_MAX - 1) == UINT32_MAX - 1);
    assert(!droid_lkm_userns_map_ok(&ns, UINT32_MAX));
    assert(!memcmp(&ns.uns.uid_map, &ns.uns.gid_map, sizeof(ns.uns.uid_map)));
    ns.uns.uid_map.nr_extents = 1;
    ns.uns.uid_map.extent[0] = (struct uid_gid_extent){100, 1000, 3};
    assert(droid_lkm_userns_map_down(&ns, 100) == 1000);
    assert(droid_lkm_userns_map_down(&ns, 102) == 1002);
    assert(droid_lkm_userns_map_down(&ns, 103) == UINT32_MAX);
    assert(droid_lkm_userns_map_up(&ns, 1002) == 102);
    assert(droid_lkm_map_id_range_down(&ns.uns.uid_map, 101, 2) == 1001);
    assert(droid_lkm_map_id_range_down(&ns.uns.uid_map, 101, 3) == UINT32_MAX);
    droid_lkm_userns_map_selftest();
    char out[4] = {0};
    loff_t pos = 0;
    assert(droid_lkm_misc_proc_read_text(out, 3, &pos, "abcdef", 6) == 3);
    assert(pos == 3 && !memcmp(out, "abc", 3));
    assert(droid_lkm_misc_proc_read_text(out, 3, &pos, "abcdef", 6) == 3);
    assert(pos == 6 && !memcmp(out, "def", 3));
    assert(droid_lkm_misc_proc_read_text(out, 3, &pos, "abcdef", 6) == 0);
    pos = -1;
    assert(droid_lkm_misc_proc_read_text(out, 3, &pos, "abcdef", 6) == -EINVAL);
    puts("PASS: identity parser, maps, proc partial reads and A64 callback alignment (ASan/UBSan)");
    return 0;
}
"""
with tempfile.TemporaryDirectory(prefix="dlkm-userns-unit-") as directory:
    source = Path(directory) / "unit.c"
    binary = Path(directory) / "unit"
    source.write_text(stubs + capability_type + "\n" + maps + parser + read_text + text_address + checks)
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra",
                    "-Werror", "-fsanitize=address,undefined", "-g", str(source),
                    "-o", str(binary)], check=True)
    environment = os.environ.copy()
    # These pure helpers allocate nothing; ptrace sandboxes cannot run LSan.
    environment["ASAN_OPTIONS"] = environment.get("ASAN_OPTIONS", "") + ":detect_leaks=0"
    subprocess.run([str(binary)], env=environment, check=True)
