# Host side of the audio graph bench (see run_graph.sh and graph_bench.cpp). Unicorn Cortex-M4F emulation. Run with python -I.
#   run  <elf> <toolchain-prefix> <scenario> <blocks> <out.bin>
#   cmp  <a.bin> <b.bin>
#   cycles <elf> <toolchain-prefix> <scenario> <warmup-blocks>
# Cycle numbers: Thumb-2 instruction count mapped through the cost table of tools/phz_cycles/run_bench.py (loads and stores 2, taken branches 3,
# vdiv 14, vmla 3 ...). NO flash wait states, no SDRAM wait states, no cache: an emulated lower bound, not a device measurement.
# SDRAM (0x70000000..) reads and writes are counted separately so a latency can be applied by the caller.
import hashlib, os, struct, subprocess, sys
from collections import defaultdict

BASE, SIZE = 0x20000000, 0xC00000
SDR, SDR_SIZE = 0x70000000, 0x1000000
TRAP = BASE + SIZE - 0x1000
BLOCK = 128 * 180e6 / 44117.647
KB = 3072


def sh(*a):
    return subprocess.run(a, check=True, capture_output=True, text=True).stdout


def load(elf, tc):
    from unicorn import UC_ARCH_ARM, UC_MODE_THUMB, Uc
    from unicorn.arm_const import UC_CPU_ARM_CORTEX_A15, UC_ARM_REG_C1_C0_2, UC_ARM_REG_FPEXC
    subprocess.run([tc + "-objcopy", "-O", "binary", "--gap-fill=0", elf, elf + ".bin"], check=True)
    image = open(elf + ".bin", "rb").read()
    syms, ranges = {}, []
    for line in sh(tc + "-nm", "-S", "-C", "--defined-only", elf).splitlines():
        p = line.split(None, 3)
        if len(p) == 4:
            a, n = int(p[0], 16), int(p[1], 16)
            syms[p[3]] = a
            if p[2] in "tTwW":
                ranges.append((a, a + n, p[3]))
    ranges.sort()
    uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
    uc.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_A15)
    uc.mem_map(BASE, SIZE)
    uc.mem_map(SDR, SDR_SIZE)
    uc.mem_write(BASE, image)
    uc.mem_write(TRAP, b"\xfe\xe7")
    uc.reg_write(UC_ARM_REG_C1_C0_2, uc.reg_read(UC_ARM_REG_C1_C0_2) | (0xF << 20))
    uc.reg_write(UC_ARM_REG_FPEXC, 0x40000000)
    return uc, syms, ranges


def call(uc, addr, arg):
    from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_R0, UC_ARM_REG_SP
    uc.reg_write(UC_ARM_REG_SP, TRAP - 0x1000)
    uc.reg_write(UC_ARM_REG_LR, TRAP | 1)
    uc.reg_write(UC_ARM_REG_R0, arg)
    uc.emu_start(addr | 1, TRAP)


def find(syms, name):
    for k in syms:
        if k == name or k.startswith(name + "("):
            return syms[k]
    raise KeyError(name)


def dump(uc, syms, n):
    def rd(sym, size):
        return bytes(uc.mem_read(syms[sym], size))
    parts = [("out", rd("gOutLog", n * 512)), ("queues", rd("gQBlockHash", n * 4)), ("meters", rd("gMeter", ((n >> 3) + 1) * 96)),
             ("misc", rd("gMisc", n)), ("stats", rd("gStats", 64))]
    return parts


def cmd_run(elf, tc, sc, n, out):
    uc, syms, _ = load(elf, tc)
    call(uc, find(syms, "bench_setup"), int(sc))
    call(uc, find(syms, "bench_run"), int(n))
    parts = dump(uc, syms, int(n))
    with open(out, "wb") as f:
        for name, data in parts:
            f.write(struct.pack("<8sI", name.encode(), len(data)) + data)
    st = struct.unpack("<16I", parts[4][1])
    print(f"{os.path.basename(elf)} sc={sc}: {st[0]} blocks, {st[1]} silent output blocks, {st[2]} export blocks, pool used {st[3]} (max {st[4]})"
          + (f", synth send connect() {st[9]} of {st[8]} accepted" if st[8] else ""))
    for name, data in parts[:4]:
        print(f"  {name:8s} sha256 {hashlib.sha256(data).hexdigest()}")


def read_bin(path):
    d, o = {}, open(path, "rb").read()
    i = 0
    while i < len(o):
        name, ln = struct.unpack_from("<8sI", o, i)
        i += 12
        d[name.rstrip(b"\0").decode()] = o[i:i + ln]
        i += ln
    return d


def cmd_cmp(pa, pb):
    a, b = read_bin(pa), read_bin(pb)
    ok = True
    for name in ("out", "queues", "meters", "misc"):
        same = a[name] == b[name]
        ok &= same
        extra = ""
        if not same and name == "out":
            n = len(a[name]) // 512
            s = struct.unpack(f"<{len(a[name]) // 2}h", a[name])
            t = struct.unpack(f"<{len(b[name]) // 2}h", b[name])
            bad = sorted({i // 256 for i in range(len(s)) if s[i] != t[i]})
            md = max(abs(s[i] - t[i]) for i in range(len(s)))
            extra = f"  {len(bad)} of {n} blocks differ, first at block {bad[0]}, max |diff| {md} LSB"
        elif not same and name == "queues":
            s = struct.unpack(f"<{len(a[name]) // 4}I", a[name])
            t = struct.unpack(f"<{len(b[name]) // 4}I", b[name])
            bad = [i for i in range(len(s)) if s[i] != t[i]]
            extra = f"  {len(bad)} blocks differ, first at block {bad[0]}"
        elif not same and name == "misc":
            extra = f"  {sum(1 for i in range(len(a[name])) if a[name][i] != b[name][i])} blocks differ (delay isSilent)"
        elif not same:
            s = struct.unpack(f"<{len(a[name]) // 4}f", a[name])
            t = struct.unpack(f"<{len(b[name]) // 4}f", b[name])
            extra = f"  {sum(1 for i in range(len(s)) if s[i] != t[i])} values differ"
        print(f"  {name:8s} {'IDENTICAL' if same else 'DIFFERENT'} ({len(a[name])} B){extra}")
    sa = struct.unpack("<16I", a["stats"])
    sb = struct.unpack("<16I", b["stats"])
    print(f"  stats    a: silent output blocks {sa[1]}, export blocks {sa[2]}, pool used {sa[3]}/{sa[4]}   b: {sb[1]}, {sb[2]}, {sb[3]}/{sb[4]}")
    print("RESULT:", "BIT IDENTICAL" if ok else "NOT identical")
    sys.exit(0 if ok else 1)


def cost(mn, ops, taken):
    base = mn.split(".")[0]
    if base.startswith("b") and base not in ("bic", "bics", "bfc", "bfi", "bkpt", "bxj", "bl", "blx", "bx") or base in ("cbz", "cbnz"):
        return 3 if taken else 1
    if base in ("bl", "blx", "bx"):
        return 3
    if base in ("vdiv", "vsqrt"):
        return 14
    if base in ("vmla", "vmls", "vnmla", "vnmls", "vfma", "vfms", "vfnma", "vfnms"):
        return 3
    if base in ("vldr", "vstr", "ldr", "ldrb", "ldrh", "ldrsb", "ldrsh", "ldrd", "str", "strb", "strh", "strd"):
        return 2
    if base in ("push", "pop", "ldm", "ldmia", "stm", "stmia", "stmdb", "ldmdb", "vpush", "vpop", "vldmia", "vstmia", "vldmdb", "vstmdb"):
        return 1 + ops.count(",") + 1 + (2 if "pc" in ops else 0)
    if base in ("sdiv", "udiv"):
        return 6
    if base in ("mla", "mls", "smlal", "umlal"):
        return 2
    return 1


def cmd_cycles(elf, tc, sc, warm):
    from capstone import CS_ARCH_ARM, CS_MODE_THUMB, Cs
    from unicorn import UC_HOOK_MEM_READ, UC_HOOK_MEM_WRITE
    from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_R2, UC_ARM_REG_SP
    uc, syms, ranges = load(elf, tc)
    call(uc, find(syms, "bench_setup"), int(sc))
    call(uc, find(syms, "bench_run"), int(warm))
    uc.ctl_flush_tb()   # translated blocks of the native warm-up run would swallow the count=1 single steps below
    cnt = {"r": 0, "w": 0}
    per_r, per_w = defaultdict(int), defaultdict(int)
    cur = [None]

    def hr(uc_, access, addr, size, value, ud):
        cnt["r"] += 1
        per_r[cur[0]] += 1

    def hw(uc_, access, addr, size, value, ud):
        cnt["w"] += 1
        per_w[cur[0]] += 1
    uc.hook_add(UC_HOOK_MEM_READ, hr, begin=SDR, end=SDR + SDR_SIZE - 1)
    uc.hook_add(UC_HOOK_MEM_WRITE, hw, begin=SDR, end=SDR + SDR_SIZE - 1)

    def func_of(addr):
        lo_i, hi_i = 0, len(ranges) - 1
        while lo_i <= hi_i:
            mid = (lo_i + hi_i) // 2
            lo, hi, name = ranges[mid]
            if addr < lo:
                hi_i = mid - 1
            elif addr >= hi:
                lo_i = mid + 1
            else:
                return name
        return "?"
    md = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
    cache = {}

    def info(addr):
        if addr not in cache:
            ins = next(md.disasm(bytes(uc.mem_read(addr, 4)), addr))
            cache[addr] = (ins.size, ins.mnemonic, ins.op_str)
        return cache[addr]
    NB = 3
    trace = []
    lrs = []   # (callee name, caller function) for memcpy and memset entries
    uc.reg_write(UC_ARM_REG_SP, TRAP - 0x1000)
    uc.reg_write(UC_ARM_REG_LR, TRAP | 1)
    uc.reg_write(UC_ARM_REG_R0, NB)
    pc = find(syms, "bench_run") | 1
    while (pc & ~1) != TRAP:
        cur[0] = pc & ~1
        if (pc & ~1) in (syms.get("memset"), syms.get("memcpy")):
            lrs.append(("memset" if (pc & ~1) == syms.get("memset") else "memcpy", uc.reg_read(UC_ARM_REG_LR) & ~1, uc.reg_read(UC_ARM_REG_R2)))
        trace.append(pc & ~1)
        uc.emu_start(pc, TRAP, count=1)
        pc = uc.reg_read(UC_ARM_REG_PC) | 1
    callers = defaultdict(int)
    for nm in ("memset", "memcpy"):
        entry = syms.get(nm)
        if entry is not None:
            pass
    tot = 0.0
    per = defaultdict(float)
    for i, a in enumerate(trace):
        size, mn, ops = info(a)
        fn = func_of(a)
        taken = i + 1 < len(trace) and trace[i + 1] != a + size
        c = cost(mn, ops, taken)
        if fn.startswith("bench_") or fn.startswith("(anonymous namespace)") or fn.startswith("SrcVoice") or fn in ("memset", "memcpy") and False:
            continue
        tot += c
        per[fn] += c
    # SDRAM accesses of the library functions only
    r_lib = sum(v for k, v in per_r.items() if not func_of(k).startswith(("bench_", "(anonymous")))
    w_lib = sum(v for k, v in per_w.items() if not func_of(k).startswith(("bench_", "(anonymous")))
    print(f"{os.path.basename(elf)} sc={sc}: {tot / NB:.0f} cycles per block = {100 * tot / NB / BLOCK:.2f} % of a block (emulated, no wait states)")
    print(f"  SDRAM per block: {r_lib / NB:.0f} reads, {w_lib / NB:.0f} writes")
    if os.environ.get("CALLERS"):
        cc = defaultdict(lambda: [0, 0])
        for nm, lr, n in lrs:
            e = cc[(nm, func_of(lr))]
            e[0] += 1
            e[1] += n
        for (nm, fn), (k, nbytes) in sorted(cc.items(), key=lambda kv: -kv[1][1]):
            print(f"    {nm:6s} {k / NB:5.1f} calls/block {nbytes / NB:8.0f} B/block  from {fn[:70]}")
    grp = defaultdict(float)
    for fn, c in per.items():
        grp[fn.split("::")[0] if "::" in fn else fn] += c
    for fn, c in sorted(grp.items(), key=lambda kv: -kv[1])[:14]:
        r = sum(v for k, v in per_r.items() if func_of(k).split("::")[0] == fn)
        w = sum(v for k, v in per_w.items() if func_of(k).split("::")[0] == fn)
        print(f"    {c / NB:>9.0f}  {fn[:60]:60s} sdram r/w {r / NB:.0f}/{w / NB:.0f}")


if __name__ == "__main__":
    m = sys.argv[1]
    if m == "run":
        cmd_run(*sys.argv[2:7])
    elif m == "cmp":
        cmd_cmp(*sys.argv[2:4])
    elif m == "cycles":
        cmd_cycles(*sys.argv[2:6])
