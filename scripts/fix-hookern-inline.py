#!/usr/bin/env python3
"""Generate a reviewed overlay for pinned HooKern ARM64 relocation code.

Never edit the fetched dependency: Kbuild compiles this generated copy instead.
Refuse source drift so a dependency update cannot silently drop these fixes.
"""
import argparse
from pathlib import Path


def replace_function(source, name, replacement):
    start = source.index("static __nocfi int " + name + "(")
    opening = source.index("{", start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[:start] + replacement + source[end:]


def fixed_source(source):
    # These signatures identify the reviewed dependency implementation.
    for expected in ["hk_sext(((insn >> 5) & 0x7FFFF) |",
                     "c->dst[c->count++] = HK_INS_BLR_X17;",
                     "6, 8, 6, 4, 4,"]:
        if expected not in source:
            raise ValueError("HooKern relocation source changed; review the overlay")
    source = replace_function(source, "hk_relo_adr", r'''static __nocfi int hk_relo_adr(struct hk_relo_ctx *c, u32 insn,
			       hk_inst_type_t type)
{
	u32 xd = insn & 0x1F;
	long imm = hk_sext((((insn >> 5) & 0x7FFFF) << 2) |
			   ((insn >> 29) & 0x3), 21);
	u64 addr;

	if (type == HK_INST_ADR) {
		addr = c->inst_addr + imm;
	} else {
		/* ADRP encodes a signed page offset, not a byte offset.
		 * Multiply rather than shifting a negative signed value. */
		addr = (c->inst_addr & ~0xFFFULL) + imm * 4096;
		if (hk_in_tramp(c, addr))
			return -EOPNOTSUPP;
	}
	c->dst[c->count++] = 0x58000040 | xd;
	c->dst[c->count++] = 0x14000003;
	c->dst[c->count++] = addr & 0xFFFFFFFF;
	c->dst[c->count++] = addr >> 32;
	return 0;
}''')
    return replace_function(source, "hk_relo_bl", r'''static __nocfi int hk_relo_bl(struct hk_relo_ctx *c, u32 insn)
{
	u64 addr = c->inst_addr + hk_sext(insn & 0x03FFFFFF, 26) * 4;

	addr = hk_relo_in_tramp(c, addr);
	/* The callee returns to the branch, which skips both literal words.
	 * Keep six emitted words, matching hk_relo_len[]. */
	c->dst[c->count++] = 0x58000071; /* ldr x17, +12 */
	c->dst[c->count++] = HK_INS_BLR_X17;
	c->dst[c->count++] = 0x14000003; /* b +12 */
	c->dst[c->count++] = addr & 0xFFFFFFFF;
	c->dst[c->count++] = addr >> 32;
	c->dst[c->count++] = HK_INS_NOP;
	return 0;
}''')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    result = fixed_source(args.source.read_text())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if not args.output.exists() or args.output.read_text() != result:
        args.output.write_text(result)


if __name__ == "__main__":
    main()
