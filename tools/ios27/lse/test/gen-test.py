#!/usr/bin/env python3
"""Generate lsetest.s / cases.h: one harness function per LSE instruction variant."""
import sys
out = sys.argv[1]

SZ = {0: 'b', 1: 'h', 2: '', 3: ''}


def reg(n, size, sp_ok=False):
    if n == 31:
        return ('sp' if sp_ok else ('xzr' if size == 3 else 'wzr'))
    return ('x' if size == 3 else 'w') + str(n)


cases = []   # (asm, kind, size, rs, rt, rn)
ORD = ['', 'a', 'l', 'al']
for size in range(4):
    for o in ORD:
        for rs, rt, rn in [(0, 1, 2), (9, 10, 11), (16, 17, 15), (1, 1, 2), (31, 1, 2), (1, 31, 2), (0, 1, 31),
                           (17, 9, 3), (2, 3, 2), (12, 13, 14), (30, 29, 0)]:
            m = f'cas{o}{SZ[size]} {reg(rs, size)}, {reg(rt, size)}, [{reg(rn, 3, True)}]'
            cases.append((m, 0, size, rs, rt, rn))
for sz in (2, 3):
    for o in ORD:
        for rs, rt, rn in [(0, 2, 4), (10, 12, 14), (16, 8, 31), (2, 2, 4), (8, 16, 10), (4, 6, 17)]:
            m = (f'casp{o} {reg(rs, sz)}, {reg(rs + 1, sz)}, {reg(rt, sz)}, {reg(rt + 1, sz)}, '
                 f'[{reg(rn, 3, True)}]')
            cases.append((m, 1, sz, rs, rt, rn))
OPS = ['add', 'clr', 'eor', 'set', 'smax', 'smin', 'umax', 'umin']
for op in OPS + ['swp']:
    for size in range(4):
        for o in ORD:
            for rs, rt, rn in [(0, 1, 2), (1, 1, 2), (3, 31, 2), (9, 10, 11), (16, 17, 31), (5, 2, 2),
                               (31, 1, 2), (14, 15, 13), (30, 29, 0)]:
                if op == 'swp':
                    m = f'swp{o}{SZ[size]} {reg(rs, size)}, {reg(rt, size)}, [{reg(rn, 3, True)}]'
                    kind = 3
                else:
                    m = f'ld{op}{o}{SZ[size]} {reg(rs, size)}, {reg(rt, size)}, [{reg(rn, 3, True)}]'
                    kind = 2
                cases.append((m, kind, size, rs, rt, rn))

with open(out + '/lsetest.s', 'w') as f:
    f.write('.arch armv8.1-a+lse\n.text\n.p2align 4\n')
    for i, (m, kind, size, rs, rt, rn) in enumerate(cases):
        f.write(f'.globl _case_{i}\n.p2align 2\n_case_{i}:\n')
        f.write('  stp x29, x30, [sp, #-96]!\n  stp x19, x20, [sp, #16]\n  stp x21, x22, [sp, #32]\n'
                '  stp x23, x24, [sp, #48]\n  stp x25, x26, [sp, #64]\n  stp x27, x28, [sp, #80]\n')
        f.write('  mov x28, x0\n  mov x9, sp\n  str x9, [x28, #264]\n')
        f.write('  ldr x9, [x28, #248]\n  cbz x9, 1f\n  mov sp, x9\n1:\n')
        f.write('  ldr x9, [x28, #256]\n  msr nzcv, x9\n')
        for r in range(0, 28, 2):
            if r == 18:
                f.write(f'  ldr x19, [x28, #152]\n')
            else:
                f.write(f'  ldp x{r}, x{r + 1}, [x28, #{8 * r}]\n')
        f.write('  ldp x29, x30, [x28, #232]\n')
        f.write(f'  {m}\n')
        for r in range(0, 28, 2):
            if r == 18:
                f.write(f'  str x19, [x28, #152]\n')
            else:
                f.write(f'  stp x{r}, x{r + 1}, [x28, #{8 * r}]\n')
        f.write('  stp x29, x30, [x28, #232]\n')
        f.write('  mrs x9, nzcv\n  str x9, [x28, #256]\n  mov x9, sp\n  str x9, [x28, #248]\n')
        f.write('  ldr x9, [x28, #264]\n  mov sp, x9\n')
        f.write('  ldp x19, x20, [sp, #16]\n  ldp x21, x22, [sp, #32]\n  ldp x23, x24, [sp, #48]\n'
                '  ldp x25, x26, [sp, #64]\n  ldp x27, x28, [sp, #80]\n  ldp x29, x30, [sp], #96\n  ret\n')
    # stress kernels (x0 = pointer, x1 = iterations)
    f.write('''
.globl _st_ldaddal
.p2align 2
_st_ldaddal:
  mov x2, #1
1: ldaddal x2, x3, [x0]
  subs x1, x1, #1
  b.ne 1b
  ret
.globl _st_casal_inc
.p2align 2
_st_casal_inc:
1: ldr x2, [x0]
2: add x3, x2, #1
  mov x4, x2
  casal x4, x3, [x0]
  cmp x4, x2
  mov x2, x4
  b.ne 2b
  subs x1, x1, #1
  b.ne 1b
  ret
.globl _st_caspal_inc
.p2align 2
_st_caspal_inc:          // 128-bit counter {lo, hi = ~lo}
1: ldp x2, x3, [x0]
2: add x4, x2, #1
  mvn x5, x4
  mov x6, x2
  mov x7, x3
  caspal x6, x7, x4, x5, [x0]
  cmp x6, x2
  ccmp x7, x3, #0, eq
  mov x2, x6
  mov x3, x7
  b.ne 2b
  subs x1, x1, #1
  b.ne 1b
  ret
.globl _st_swpal_lock
.p2align 2
_st_swpal_lock:          // x0 = {lock, counter}: spinlock with swpa / stlr, plain counter inside
  mov w5, #1
1: swpa w5, w6, [x0]
  cbnz w6, 1b
  ldr x7, [x0, #8]
  add x7, x7, #1
  str x7, [x0, #8]
  stlr wzr, [x0]
  subs x1, x1, #1
  b.ne 1b
  ret
.globl _st_ldsmaxal
.p2align 2
_st_ldsmaxal:            // x0 ptr, x1 iterations, x2 start value (thread-distinct)
1: ldsmaxal x2, x3, [x0]
  add x2, x2, #4
  subs x1, x1, #1
  b.ne 1b
  ret
.globl _stub_pool
.p2align 6
_stub_pool:
  .space 0x100000
.globl _stub_pool_end
_stub_pool_end:
  ret
''')
with open(out + '/cases.h', 'w') as f:
    f.write('#define NCASES %d\n' % len(cases))
    for i in range(len(cases)):
        f.write(f'extern void case_{i}(unsigned long *);\n')
    f.write('static const struct tc { void (*fn)(unsigned long *); int kind, size, rs, rt, rn; const char *name; } T[] = {\n')
    for i, (m, kind, size, rs, rt, rn) in enumerate(cases):
        f.write(f'  {{case_{i}, {kind}, {size}, {rs}, {rt}, {rn}, "{m}"}},\n')
    f.write('};\n')
print(len(cases), 'cases')
