// SPDX-License-Identifier: GPL-2.0-only
/* Unmodified relocation helpers from HooKern 3f1cdd0, for offline tests.
 * Original hk_relo_len starts with: 6, 8, 6, 4, 4, */
static __nocfi long hk_sext(u64 v, int bits)
{
	if (bits < 64 && (v & (1ULL << (bits - 1))))
		v |= ~0ULL << bits;
	return (long)v;
}
static bool hk_in_tramp(const struct hk_relo_ctx *c, u64 addr)
{
	return addr >= c->tramp_start && addr < c->tramp_end;
}
static __nocfi int hk_relo_bl(struct hk_relo_ctx *c, u32 insn)
{
	u64 addr = c->inst_addr + hk_sext(insn & 0x03FFFFFF, 26) * 4;

	addr = hk_relo_in_tramp(c, addr);
	c->dst[c->count++] = HK_INS_LDR_X17;
	c->dst[c->count++] = HK_INS_BLR_X17;
	c->dst[c->count++] = addr & 0xFFFFFFFF;
	c->dst[c->count++] = addr >> 32;
	c->dst[c->count++] = HK_INS_NOP;
	c->dst[c->count++] = HK_INS_NOP;
	return 0;
}
static __nocfi int hk_relo_adr(struct hk_relo_ctx *c, u32 insn,
			       hk_inst_type_t type)
{
	u32 xd = insn & 0x1F;
	u64 addr;

	if (type == HK_INST_ADR) {
		addr = c->inst_addr + hk_sext(((insn >> 5) & 0x7FFFF) |
					      ((insn >> 29) & 0x3), 21);
	} else {
		addr = (c->inst_addr & ~0xFFFUL) +
		       hk_sext((((insn >> 5) & 0x7FFFF) << 2) |
			       ((insn >> 29) & 0x3), 21);
		if (hk_in_tramp(c, addr))
			return -EOPNOTSUPP;
	}
	c->dst[c->count++] = 0x58000040 | xd;
	c->dst[c->count++] = 0x14000003;
	c->dst[c->count++] = addr & 0xFFFFFFFF;
	c->dst[c->count++] = addr >> 32;
	return 0;
}
