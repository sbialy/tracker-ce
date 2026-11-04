# Idle synth part bench, host side (see run_bench.sh): runs bench_run() of bench_idle.cpp in a Unicorn Cortex-M4F emulator, counts
# the executed Thumb-2 instructions (single stepped) and converts them to cycles with the cost table of tools/phz_cycles/run_bench.py
# (no flash wait states, no cache). Not cycle accurate. Run with python -I.
import struct, subprocess, sys
from collections import defaultdict
from capstone import CS_ARCH_ARM, CS_MODE_THUMB, Cs
from unicorn import UC_ARCH_ARM, UC_MODE_THUMB, Uc
from unicorn.arm_const import (UC_CPU_ARM_CORTEX_A15, UC_ARM_REG_C1_C0_2, UC_ARM_REG_FPEXC, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_SP)
ELF, TC = sys.argv[1], sys.argv[2]
BASE, SIZE = 0x20000000, 0x400000
TRAP = BASE + 0x3F0000
BLOCK = 128 * 180e6 / 44117.647
NB = 16
def sh(*a): return subprocess.run(a, check=True, capture_output=True, text=True).stdout
subprocess.run([TC + "-objcopy", "-O", "binary", ELF, ELF + ".bin"], check=True)
image = open(ELF + ".bin", "rb").read()
syms, ranges = {}, []
for line in sh(TC + "-nm", "-S", "-C", "--defined-only", ELF).splitlines():
    p = line.split(None, 3)
    if len(p) == 4 and p[2] in "tTwW":
        a, n = int(p[0], 16), int(p[1], 16); syms[p[3]] = a; ranges.append((a, a + n, p[3]))
ranges.sort()
def func_of(addr):
    for lo, hi, name in ranges:
        if lo <= addr < hi: return name
    return "?"
uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB); uc.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_A15)
uc.mem_map(BASE, SIZE); uc.mem_write(BASE, image); uc.mem_write(TRAP, b"\xfe\xe7")
uc.reg_write(UC_ARM_REG_C1_C0_2, uc.reg_read(UC_ARM_REG_C1_C0_2) | (0xF << 20)); uc.reg_write(UC_ARM_REG_FPEXC, 0x40000000)
def find(prefix):
    for k in syms:
        if k == prefix or k.startswith(prefix + "("): return k
    raise KeyError(prefix)
def stepped(fn, arg, visit=None):
    uc.reg_write(UC_ARM_REG_SP, BASE + SIZE - 0x10000); uc.reg_write(UC_ARM_REG_LR, TRAP | 1); uc.reg_write(UC_ARM_REG_R0, arg)
    pc = syms[fn] | 1
    while (pc & ~1) != TRAP:
        if visit: visit(pc & ~1)
        uc.emu_start(pc, TRAP, count=1); pc = uc.reg_read(UC_ARM_REG_PC) | 1
md = Cs(CS_ARCH_ARM, CS_MODE_THUMB); _c = {}
def info(addr):
    if addr not in _c:
        ins = next(md.disasm(bytes(uc.mem_read(addr, 4)), addr)); _c[addr] = (ins.size, ins.mnemonic, ins.op_str)
    return _c[addr]
def cost(mn, ops, taken):
    base = mn.split(".")[0]
    if base.startswith("b") and base not in ("bic","bics","bfc","bfi","bkpt","bxj","bl","blx","bx") or base in ("cbz","cbnz"): return 3 if taken else 1
    if base in ("bl","blx","bx"): return 3
    if base in ("vdiv","vsqrt"): return 14
    if base in ("vmla","vmls","vnmla","vnmls","vfma","vfms","vfnma","vfnms"): return 3
    if base in ("vldr","vstr","ldr","ldrb","ldrh","ldrsb","ldrsh","ldrd","str","strb","strh","strd"): return 2
    if base in ("push","pop","ldm","ldmia","stm","stmia","stmdb","ldmdb","vpush","vpop","vldmia","vstmia","vldmdb","vstmdb"): return 1 + ops.count(",") + 1 + (2 if "pc" in ops else 0)
    if base in ("sdiv","udiv"): return 6
    if base in ("mla","mls","smlal","umlal"): return 2
    return 1
NS, NR = find("bench_setup"), find("bench_run")
import os
sc = int(os.environ.get("SC", "1"))
stepped(NS, sc)
stepped(NR, 6)   # warm up
trace = []
stepped(NR, NB, trace.append)
tot = 0; per = defaultdict(float)
for i, a in enumerate(trace):
    size, mn, ops = info(a)
    if func_of(a) == "bench_run": continue
    taken = i + 1 < len(trace) and trace[i + 1] != a + size
    c = cost(mn, ops, taken); tot += c; per[func_of(a)] += c
print(f"{os.path.basename(ELF)} sc={sc}: {tot / NB:.0f} cycles per cycle of update_all = {100 * tot / NB / BLOCK:.2f} % of a block")
for fn, c in sorted(per.items(), key=lambda kv: -kv[1])[:9]: print(f"    {c / NB:>8.0f}  {fn[:90]}")
