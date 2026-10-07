#!/usr/bin/env python3
"""Exercise the compiled relocation overlay, including the phone's opcodes."""
import importlib.util
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("overlay", ROOT / "scripts/fix-hookern-inline.py")
overlay = importlib.util.module_from_spec(spec)
spec.loader.exec_module(overlay)
fixture = (ROOT / "tests/fixtures/hookern-inline-3f1cdd0.c").read_text()

stubs = r'''
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef uint32_t u32;
typedef uint64_t u64;
#define __nocfi
#define HK_INS_LDR_X17 0x58000051
#define HK_INS_BLR_X17 0xD63F0220
#define HK_INS_NOP 0xD503201F
typedef enum { HK_INST_ADR, HK_INST_ADRP } hk_inst_type_t;
struct hk_relo_ctx {
    u32 *dst, count;
    unsigned long inst_addr, tramp_start, tramp_end;
};
static u64 hk_relo_in_tramp(struct hk_relo_ctx *c, u64 addr) {
    (void)c; return addr;
}
'''
checks = r'''
static u64 literal(u32 *out, unsigned int pos) {
    return ((u64)out[pos + 1] << 32) | out[pos];
}
static u32 adr_opcode(long imm, bool page) {
    u32 bits = (u32)imm & 0x1fffff;
    return (page ? 0x90000000U : 0x10000000U) | 2 |
           ((bits & 3) << 29) | ((bits >> 2) << 5);
}
static void check_adr(unsigned long pc, u32 insn, u64 want, bool page) {
    u32 out[8] = {0};
    struct hk_relo_ctx c = {.dst = out, .inst_addr = pc};
    assert(!hk_relo_adr(&c, insn, page ? HK_INST_ADRP : HK_INST_ADR));
    assert(c.count == 4);
    assert((out[0] & 31) == (insn & 31));
    assert(literal(out, 2) == want);
}
int main(void) {
    /* Actual proc_tgid/tid prologues from the phone Image. */
    check_adr(0xffffffc0804f86f4UL, 0xd00068e2,
              0xffffffc081216000ULL, true);
    check_adr(0xffffffc0804fbc48UL, 0xb00068c2,
              0xffffffc081214000ULL, true);
    const long offsets[] = {-1048576, -65537, -4, -3, -2, -1,
                             0, 1, 2, 3, 4, 65537, 1048575};
    unsigned long pc = 0xffffffc0804f86f4UL;
    for (unsigned int i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
        long n = offsets[i];
        check_adr(pc, adr_opcode(n, false), pc + n, false);
        check_adr(pc, adr_opcode(n, true), (pc & ~4095UL) + n * 4096, true);
    }
    /* A returning BL must resume at an instruction that skips its literal.
     * Decode the emitted ARM64 offsets rather than assuming slot positions. */
    for (int disp = -64; disp <= 64; disp += 64) {
        u32 out[8] = {0};
        struct hk_relo_ctx c = {.dst = out, .inst_addr = pc};
        assert(!hk_relo_bl(&c, 0x94000000 | ((u32)(disp / 4) & 0x3ffffff)));
        assert(c.count == 6);
        assert((out[0] & 0xff00001f) == 0x58000011);
        unsigned int pool = ((out[0] >> 5) & 0x7ffff);
        assert(literal(out, pool) == pc + disp);
        assert(out[1] == HK_INS_BLR_X17);
        assert((out[2] & 0xfc000000) == 0x14000000);
        assert(2 + (out[2] & 0x3ffffff) == 5);
        assert(out[5] == HK_INS_NOP);
    }
    puts("PASS: phone ADRP opcodes, signed ADR/ADRP offsets and returning BL");
    return 0;
}
'''


def run(source, directory, name):
    path = directory / (name + ".c")
    binary = directory / name
    path.write_text(stubs + source + checks)
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-Wall", "-Wextra",
                    "-Werror", "-fsanitize=undefined", "-g", str(path), "-o", str(binary)],
                   check=True)
    return subprocess.run([str(binary)], capture_output=True, text=True)


with tempfile.TemporaryDirectory(prefix="dlkm-inline-unit-") as name:
    directory = Path(name)
    positive = run(overlay.fixed_source(fixture), directory, "fixed")
    print(positive.stdout, end="")
    if positive.returncode:
        raise RuntimeError(positive.stderr)
    negative = run(fixture, directory, "original")
    if negative.returncode == 0 or "literal(out, 2) == want" not in negative.stderr:
        raise RuntimeError("Unmodified HooKern must fail the phone ADRP regression")
    print("PASS: unmodified dependency fails the phone ADRP regression (negative control)")
    # Independently exercise the BL defect after correcting ADR/ADRP, so the
    # first ADRP assertion cannot hide a still-broken return path.
    start = fixture.index("static __nocfi int hk_relo_bl(")
    end = fixture.index("static __nocfi int hk_relo_adr(", start)
    original_bl = fixture[start:end].strip()
    adr_only = overlay.replace_function(overlay.fixed_source(fixture),
                                        "hk_relo_bl", original_bl)
    negative_bl = run(adr_only, directory, "original_bl")
    if negative_bl.returncode == 0 or "out[2] & 0xfc000000" not in negative_bl.stderr:
        raise RuntimeError("Original BL must fail the returning-call regression")
    print("PASS: original BL emitter fails returning-call regression (negative control)")
