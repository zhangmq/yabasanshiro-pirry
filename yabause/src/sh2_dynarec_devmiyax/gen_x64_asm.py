#!/usr/bin/env python3
"""Regenerate the generated half of dynalib_x64.S.

dynalib_x64.S is written in two layers: the hand-written structural helpers at
the top (prologue, epilogue, separators, block-exit jumps, fast memory paths)
and the op snippets plus their patch metadata below the "generated ops" marker.
Only the second layer comes from here: the 140 SH-2 ops of the AArch64 table
either get a real x86-64 body from OPS below, or a ud2 trap so that a block
reaching an unimplemented opcode dies visibly instead of running wild.

Sizes are measured from the assembled object (`nm -S`), so a body can be edited
without hand-counting bytes -- the emitter charges these numbers when it decides
whether a block still fits, and copies exactly that many bytes.

    python3 gen_x64_asm.py
"""
import re, subprocess
import os
HERE=os.path.dirname(os.path.abspath(__file__))
AR=os.path.join(HERE,'dynalib_arm64.s')
HDR=os.path.join(HERE,'backend','dyn_backend_x64.h')
OUT=os.path.join(HERE,'dynalib_x64.S')
names=[m.group(1) for m in re.finditer(r'\nopdesc\s+([A-Za-z0-9_]+)\s*,', open(AR).read())]
cur=open(OUT).read()
# Everything up to the first op snippet: copyright header, register notes and
# the structural helpers.  Split on the marker the generator itself emits so a
# re-run cannot drop the header.
HEAD=cur.split('/* ---- generated ops')[0]
if 'Copyright 2019 devMiyax' not in HEAD:
    raise SystemExit('refusing to rewrite: copyright block missing')
TAIL=cur[cur.index('/* Fast memory access paths'):]

SETT=lambda cc: f'  set{cc} al\n  movzx eax, al\n  and  ebx, -2\n  or   ebx, eax\n'
TWO='  mov  ecx, 0\n  mov  edx, 0\n  mov  eax, [r15+rcx*4]\n'
ONE='  mov  ecx, 0\n  mov  eax, [r15+rcx*4]\n'
ST='  mov  [r15+rdx*4], eax\n'
ST1='  mov  [r15+rcx*4], eax\n'

# Operand patch slots, matching the way opcodePass() patches an x64 snippet:
# src=Rm (opcode>>4), dest=Rn (opcode>>8), off1=(opcode&15), imm=(opcode&0xff),
# off3=(opcode&0xfff).  Each value is the byte offset of the imm32 to overwrite,
# 0xff = the op has no such operand.  The register operands are written as a
# plain register number because the bodies index the register file with a SIB
# scale of 4 ([r15+rcx*4]); the immediate is the raw 8/12-bit field.
#
# name: (prefix, body, src, dest, off1, imm, off3)
OPS={}
def add(n, pre, body, src='0xff', dest='0xff', off1='0xff', imm='0xff', off3='0xff'):
    OPS[n]=(pre, body, src, dest, off1, imm, off3)
def two(n, body): add(n, TWO, body, src='1', dest='6')
def one(n, body): add(n, ONE, body, dest='1')

two('AND','  and  dword ptr [r15+rdx*4], eax\n')
two('OR', '  or   dword ptr [r15+rdx*4], eax\n')
two('XOR','  xor  dword ptr [r15+rdx*4], eax\n')
two('SUB','  sub  dword ptr [r15+rdx*4], eax\n')
two('TST','  test eax, dword ptr [r15+rdx*4]\n'+SETT('e'))
two('CMP_EQ','  cmp dword ptr [r15+rdx*4], eax\n'+SETT('e'))
two('CMP_HS','  cmp dword ptr [r15+rdx*4], eax\n'+SETT('ae'))
two('CMP_HI','  cmp dword ptr [r15+rdx*4], eax\n'+SETT('a'))
two('CMP_GT','  cmp dword ptr [r15+rdx*4], eax\n'+SETT('g'))
two('CMP_GE','  cmp dword ptr [r15+rdx*4], eax\n'+SETT('ge'))
two('NEG','  neg  eax\n'+ST)
two('NOT','  not  eax\n'+ST)
two('SWAP_W','  rol  eax, 16\n'+ST)
two('SWAP_B','  xchg al, ah\n'+ST)
two('EXTUB','  movzx eax, byte ptr [r15+rcx*4]\n'+ST)
two('EXTS_B','  movsx eax, byte ptr [r15+rcx*4]\n'+ST)
two('EXTU_W','  movzx eax, word ptr [r15+rcx*4]\n'+ST)
two('EXTS_W','  movsx eax, word ptr [r15+rcx*4]\n'+ST)
for n,sh in (('SHL','shl'),('SHLR','shr'),('SHAR','sar'),('ROTL','rol'),('ROTR','ror')):
    one(n, f'  {sh} eax, 1\n  setc dl\n{ST1}  movzx edx, dl\n  and  ebx, -2\n  or   ebx, edx\n')
add('DT', '  mov  ecx, 0\n', '  sub  dword ptr [r15+rcx*4], 1\n  sete al\n  movzx eax, al\n  and  ebx, -2\n  or   ebx, eax\n', dest='1')
add('ADD', TWO, '  add  dword ptr [r15+rdx*4], eax\n', src='1', dest='6')

# --- arithmetic with flags -------------------------------------------------
# T is bit 0 of SR (ebx).  `bt ebx,0` puts it in CF so one adc/sbb does the
# whole Rn +/- Rm +/- T, and the carry that comes out is the new T.  ADDV/SUBV
# take seto instead: T is signed overflow, which is exactly OF of the add/sub.
add('ADDC', TWO, '  bt   ebx, 0\n  adc  eax, [r15+rdx*4]\n' + ST + SETT('c'), src='1', dest='6')
add('ADDV', TWO, '  add  eax, [r15+rdx*4]\n' + ST + SETT('o'), src='1', dest='6')
add('SUBC', TWO, '  bt   ebx, 0\n  sbb  dword ptr [r15+rdx*4], eax\n' + SETT('c'), src='1', dest='6')
add('SUBV', TWO, '  sub  dword ptr [r15+rdx*4], eax\n' + SETT('o'), src='1', dest='6')

# --- immediate forms -------------------------------------------------------
# ADDI sign-extends its #imm; the other four are logical and only touch R0.
add('ADDI', '  mov  edx, 0\n  mov  eax, 0\n', '  movsx eax, al\n  add  dword ptr [r15+rdx*4], eax\n', dest='1', imm='6')
IMM='  mov  eax, 0\n'
add('ANDI', IMM, '  and  dword ptr [r15], eax\n', imm='1')
add('ORI',  IMM, '  or   dword ptr [r15], eax\n', imm='1')
add('XORI', IMM, '  xor  dword ptr [r15], eax\n', imm='1')
add('TSTI', IMM, '  test eax, dword ptr [r15]\n' + SETT('e'), imm='1')
add('CMP_EQ_IMM', IMM, '  movsx eax, al\n  cmp  dword ptr [r15], eax\n' + SETT('e'), imm='1')
# CMP/PZ: T = (Rn >= 0), i.e. the sign flag clear (the AArch64 body tests
# bit 31).  'e' would be CMP/EQ and leaves a positive value with T=0.
add('CMP_PZ', '  mov  edx, 0\n', '  cmp  dword ptr [r15+rdx*4], 0\n' + SETT('ns'), dest='1')
add('CMP_PL', '  mov  edx, 0\n', '  cmp  dword ptr [r15+rdx*4], 0\n' + SETT('g'), dest='1')

# --- 2/8/16-bit-pair shifts: logical, T is not touched --------------------
for n,sh,amt in (('SHLL2','shl',2),('SHLL8','shl',8),('SHLL16','shl',16),
                 ('SHLR2','shr',2),('SHLR8','shr',8),('SHLR16','shr',16)):
    one(n, f'  {sh} eax, {amt}\n' + ST1)

# --- rotate through T ------------------------------------------------------
# T is the bit that comes in; the bit that falls out becomes the new T.
ROTX=ST1 + '  setc dl\n  movzx edx, dl\n  and  ebx, -2\n  or   ebx, edx\n'
one('ROTCL', '  bt   ebx, 0\n  rcl  eax, 1\n' + ROTX)
one('ROTCR', '  bt   ebx, 0\n  rcr  eax, 1\n' + ROTX)

# --- multiplies: the product always lands in MACL/MACH, never in Rn --------
MAC='  mov  [r15+80], eax\n'          # MACL
MACHI='  shr  rax, 32\n  mov  [r15+76], eax\n'   # MACH = high half
add('MULS', TWO, '  movsx eax, word ptr [r15+rcx*4]\n  movsx edx, word ptr [r15+rdx*4]\n  imul eax, edx\n' + MAC, src='1', dest='6')
add('MULU', TWO, '  movzx eax, word ptr [r15+rcx*4]\n  movzx edx, word ptr [r15+rdx*4]\n  imul eax, edx\n' + MAC, src='1', dest='6')
add('MULL', TWO, '  mov  eax, [r15+rcx*4]\n  mov  edx, [r15+rdx*4]\n  imul eax, edx\n' + MAC, src='1', dest='6')
add('DMULS', TWO, '  movsxd rax, dword ptr [r15+rcx*4]\n  movsxd rdx, dword ptr [r15+rdx*4]\n  imul rax, rdx\n' + MAC + MACHI, src='1', dest='6')
add('DMULU', TWO, '  mov  eax, [r15+rcx*4]\n  mov  edx, [r15+rdx*4]\n  imul rax, rdx\n' + MAC + MACHI, src='1', dest='6')

# --- memory: the C helpers take (address in edi, value in esi) and return in eax
# (SysV).  rax/rcx/rdx/rsi/rdi are caller-saved, so an operand register number
# needed after the call is kept with a pair of pushes: two pushes keep rsp a
# multiple of 16 at the call, which the ABI requires.  Register numbers that are
# only needed after the call could be loaded late instead, but keeping one shape
# for all of them is easier to audit.
GETB='  call qword ptr [r15+104]\n'
GETW='  call qword ptr [r15+112]\n'
GETL='  call qword ptr [r15+120]\n'
SETB='  call qword ptr [r15+128]\n'
SETW='  call qword ptr [r15+136]\n'
SETL='  call qword ptr [r15+144]\n'
TWON='  mov  ecx, 0\n  mov  edx, 0\n'
KEEP=lambda r: f'  push {r}\n  push {r}\n'
DROP=lambda r: f'  pop  {r}\n  pop  {r}\n'
LOADBS='  movsx eax, al\n'
LOADWS='  movsx eax, ax\n'

# Rm -> @Rn
two2=lambda n,b: add(n, TWON, b, src='1', dest='6')
two2('MOVBS', '  mov  esi, [r15+rcx*4]\n  mov  edi, [r15+rdx*4]\n' + SETB)
two2('MOVWS', '  mov  esi, [r15+rcx*4]\n  mov  edi, [r15+rdx*4]\n' + SETW)
two2('MOVLS', '  mov  esi, [r15+rcx*4]\n  mov  edi, [r15+rdx*4]\n' + SETL)

# Rm -> @-Rn  (Rn is decremented before the store; the value is Rm, not R0)
add('MOVBM', TWON, '  sub  dword ptr [r15+rdx*4], 1\n  mov  edi, [r15+rdx*4]\n  mov  esi, [r15+rcx*4]\n' + SETB, src='1', dest='6')
add('MOVWM', TWON, '  sub  dword ptr [r15+rdx*4], 2\n  mov  edi, [r15+rdx*4]\n  mov  esi, [r15+rcx*4]\n' + SETW, src='1', dest='6')
add('MOVLM', TWON, '  sub  dword ptr [r15+rdx*4], 4\n  mov  edi, [r15+rdx*4]\n  mov  esi, [r15+rcx*4]\n' + SETL, src='1', dest='6')

# @Rm+ -> Rn  (post-increment before the read, the old value is the address)
add('MOVBP', TWON, '  mov  edi, [r15+rcx*4]\n  add  dword ptr [r15+rcx*4], 1\n' + KEEP('rdx') + GETB + DROP('rdx') + LOADBS + '  mov  [r15+rdx*4], eax\n', src='1', dest='6')
add('MOVWP', TWON, '  mov  edi, [r15+rcx*4]\n  add  dword ptr [r15+rcx*4], 2\n' + KEEP('rdx') + GETW + DROP('rdx') + LOADWS + '  mov  [r15+rdx*4], eax\n', src='1', dest='6')
add('MOVLP', TWON, '  mov  edi, [r15+rcx*4]\n  add  dword ptr [r15+rcx*4], 4\n' + KEEP('rdx') + GETL + DROP('rdx') + '  mov  [r15+rdx*4], eax\n', src='1', dest='6')

# @Rm -> Rn
add('MOVBL', TWON, '  mov  edi, [r15+rcx*4]\n' + KEEP('rdx') + GETB + DROP('rdx') + LOADBS + '  mov  [r15+rdx*4], eax\n', src='1', dest='6')
add('MOVWL', TWON, '  mov  edi, [r15+rcx*4]\n' + KEEP('rdx') + GETW + DROP('rdx') + LOADWS + '  mov  [r15+rdx*4], eax\n', src='1', dest='6')
add('MOVL_MEM_REG', TWON, '  mov  edi, [r15+rcx*4]\n' + KEEP('rdx') + GETL + DROP('rdx') + '  mov  [r15+rdx*4], eax\n', src='1', dest='6')

# register to register / T to register / immediate to register
two2('MOVR', '  mov  eax, [r15+rcx*4]\n  mov  [r15+rdx*4], eax\n')
add('MOVT', '  mov  edx, 0\n', '  mov  eax, ebx\n  and  eax, 1\n  mov  [r15+rdx*4], eax\n', dest='1')
add('MOVI', '  mov  edx, 0\n  mov  eax, 0\n', '  movsx eax, al\n  mov  [r15+rdx*4], eax\n', dest='1', imm='6')

# R0-relative
two2('MOVBS0', '  mov  esi, [r15+rcx*4]\n  mov  edi, [r15+rdx*4]\n  add  edi, [r15]\n' + SETB)
two2('MOVWS0', '  mov  esi, [r15+rcx*4]\n  mov  edi, [r15+rdx*4]\n  add  edi, [r15]\n' + SETW)
two2('MOVLS0', '  mov  esi, [r15+rcx*4]\n  mov  edi, [r15+rdx*4]\n  add  edi, [r15]\n' + SETL)
add('MOVBL0', TWON, '  mov  edi, [r15+rcx*4]\n  add  edi, [r15]\n' + KEEP('rdx') + GETB + DROP('rdx') + LOADBS + '  mov  [r15+rdx*4], eax\n', src='1', dest='6')
add('MOVWL0', TWON, '  mov  edi, [r15+rcx*4]\n  add  edi, [r15]\n' + KEEP('rdx') + GETW + DROP('rdx') + LOADWS + '  mov  [r15+rdx*4], eax\n', src='1', dest='6')
add('MOVLL0', TWON, '  mov  edi, [r15+rcx*4]\n  add  edi, [r15]\n' + KEEP('rdx') + GETL + DROP('rdx') + '  mov  [r15+rdx*4], eax\n', src='1', dest='6')

# @(disp,Rn) forms.  0x8000/0x8100 keep the base register in bits 7-4 (the
# 0x8n0d view would restrict the dispatch mask to r0), 0x1000/0x5000 use the
# nmd layout: m in bits 7-4, n in bits 11-8, disp in bits 3-0.
DISP='  mov  edi, 0\n'
add('MOVBS4', '  mov  ecx, 0\n' + DISP, '  add  edi, [r15+rcx*4]\n  mov  esi, [r15]\n' + SETB, src='1', off1='6')
add('MOVWS4', '  mov  ecx, 0\n' + DISP, '  add  edi, edi\n  add  edi, [r15+rcx*4]\n  mov  esi, [r15]\n' + SETW, src='1', off1='6')
add('MOVBL4', '  mov  ecx, 0\n' + DISP, '  add  edi, [r15+rcx*4]\n' + GETB + LOADBS + '  mov  [r15], eax\n', src='1', off1='6')
add('MOVWL4', '  mov  ecx, 0\n' + DISP, '  add  edi, edi\n  add  edi, [r15+rcx*4]\n' + GETW + LOADWS + '  mov  [r15], eax\n', src='1', off1='6')
add('MOVLS4', '  mov  ecx, 0\n  mov  edx, 0\n' + DISP, '  shl  edi, 2\n  add  edi, [r15+rdx*4]\n  mov  esi, [r15+rcx*4]\n' + SETL, src='1', dest='6', off1='11')
add('MOVLL4', '  mov  ecx, 0\n  mov  edx, 0\n' + DISP, '  shl  edi, 2\n  add  edi, [r15+rcx*4]\n' + KEEP('rdx') + GETL + DROP('rdx') + '  mov  [r15+rdx*4], eax\n', src='1', dest='6', off1='11')

# GBR-relative
GBR='  add  edi, [r15+68]\n'
add('MOVBSG', DISP, GBR + '  mov  esi, [r15]\n' + SETB, imm='1')
add('MOVWSG', DISP, '  add  edi, edi\n' + GBR + '  mov  esi, [r15]\n' + SETW, imm='1')
add('MOVLSG', DISP, '  shl  edi, 2\n' + GBR + '  mov  esi, [r15]\n' + SETL, imm='1')
add('MOVBLG', DISP, GBR + GETB + LOADBS + '  mov  [r15], eax\n', imm='1')
add('MOVWLG', DISP, '  add  edi, edi\n' + GBR + GETW + LOADWS + '  mov  [r15], eax\n', imm='1')
add('MOVLLG', DISP, '  shl  edi, 2\n' + GBR + GETL + '  mov  [r15], eax\n', imm='1')

# R0 = ((PC+4) & ~3) + disp*4 ; r14d holds this instruction's address
add('MOVA', '  mov  eax, 0\n', '  mov  edx, r14d\n  add  edx, 4\n  and  edx, -4\n  lea  edx, [rdx+rax*4]\n  mov  [r15], edx\n', imm='1')

# TAS.B @Rn: T = (byte == 0), then write back byte|0x80
add('TAS', '  mov  edx, 0\n', '  mov  edi, [r15+rdx*4]\n' + KEEP('rdi') + GETB + DROP('rdi') +
    '  movzx eax, al\n  test eax, eax\n  sete cl\n  movzx ecx, cl\n  and  ebx, -2\n  or   ebx, ecx\n'
    '  or   eax, 0x80\n  mov  esi, eax\n' + SETB, dest='1')

# --- T bit and MAC helpers -------------------------------------------------
add('CLRT', '', '  and  ebx, -2\n')
add('SETT', '', '  or   ebx, 1\n')
add('CLRMAC', '', '  mov  qword ptr [r15+76], 0\n')     # MACH/MACL
add('DIV0U', '', '  and  ebx, 0xFFFFFCFE\n')             # clear T, M, Q
# SLEEP keeps the PC on the instruction itself: the block is BLOCK_LOOP, so
# it spins here until the time slice runs out (same as the AArch64 backend).
add('SLEEP', '', '  sub  r14d, 2\n')

# Rn = (Rm << 16) | (Rn >> 16)
add('XTRCT', TWON, '  mov  eax, [r15+rcx*4]\n  mov  esi, [r15+rdx*4]\n'
                   '  shl  eax, 16\n  shr  esi, 16\n  or   eax, esi\n  mov  [r15+rdx*4], eax\n',
    src='1', dest='6')

# Rn = 0 - Rm - T ; T = borrow
add('NEGC', TWO, '  xor  eax, eax\n  bt   ebx, 0\n  sbb  eax, [r15+rcx*4]\n' + ST + SETT('c'), src='1', dest='6')

# T = 1 when any byte of (Rm ^ Rn) is zero.
add('CMPSTR', TWON, '  mov  eax, [r15+rcx*4]\n  xor  eax, [r15+rdx*4]\n'
                    '  mov  ecx, eax\n  sub  ecx, 0x01010101\n  not  eax\n  and  ecx, eax\n'
                    '  test ecx, 0x80808080\n  setne cl\n  movzx ecx, cl\n'
                    '  and  ebx, -2\n  or   ebx, ecx\n', src='1', dest='6')

# --- byte logic on @(R0,GBR): read, combine with #imm, write back ----------
# The immediate is needed after the read call, so it is parked on the stack
# (two pushes keep the stack 16-byte aligned at the call).
GBRA='  mov  edi, [r15]\n  add  edi, [r15+68]\n'
IMMP='  mov  eax, 0\n'
for n,opc in (('AND_B','and'),('OR_B','or'),('XOR_B','xor')):
    add(n, IMMP, KEEP('rax') + GBRA + GETB + '  movzx esi, al\n' + DROP('rax') +
        f'  {opc}  esi, eax\n' + GBRA + SETB, imm='1')
add('TST_B', IMMP, KEEP('rax') + GBRA + GETB + '  movzx eax, al\n' + DROP('rdx') +
    '  test eax, edx\n  sete al\n  movzx eax, al\n  and  ebx, -2\n  or   ebx, eax\n', imm='1')

# --- branches --------------------------------------------------------------
# The emitter appends a separator (delay == 1/5) or the delay-slot sequence
# (delay == 2/3/4); both read the target PC from eax, 0 meaning "fall through".
D8='  mov  eax, 0\n  movsx eax, al\n  lea  eax, [r14+rax*2+4]\n'
ZERO='  xor  ecx, ecx\n  test bl, 1\n'
add('BT', '  mov  eax, 0\n', '  movsx eax, al\n  lea  eax, [r14+rax*2+4]\n' + ZERO + '  cmovz eax, ecx\n', imm='1')
add('BF', '  mov  eax, 0\n', '  movsx eax, al\n  lea  eax, [r14+rax*2+4]\n' + ZERO + '  cmovnz eax, ecx\n', imm='1')
add('BF_S', '  mov  eax, 0\n', '  movsx eax, al\n  lea  eax, [r14+rax*2+4]\n' + ZERO + '  cmovnz eax, ecx\n', imm='1')
D12='  mov  eax, 0\n  shl  eax, 20\n  sar  eax, 20\n'
add('BRA', '  mov  eax, 0\n', '  shl  eax, 20\n  sar  eax, 20\n  lea  eax, [r14+rax*2+4]\n', off3='1')
add('BSR', '  mov  eax, 0\n', '  shl  eax, 20\n  sar  eax, 20\n  lea  edx, [r14+4]\n'
                              '  mov  [r15+84], edx\n  lea  eax, [rdx+rax*2]\n', off3='1')
add('JMP', '  mov  edx, 0\n', '  mov  eax, [r15+rdx*4]\n', dest='1')
add('JSR', '  mov  edx, 0\n', '  lea  ecx, [r14+4]\n  mov  [r15+84], ecx\n  mov  eax, [r15+rdx*4]\n', dest='1')
add('BRAF', '  mov  edx, 0\n', '  mov  eax, [r15+rdx*4]\n  lea  eax, [r14+rax+4]\n', dest='1')
add('BSRF', '  mov  edx, 0\n', '  lea  ecx, [r14+4]\n  mov  [r15+84], ecx\n  mov  eax, [r15+rdx*4]\n'
                               '  lea  eax, [r14+rax+4]\n', dest='1')

# RTE: PC = mem[R15], SR = mem[R15+4] & 0x3f3, R15 += 8; eax = the saved PC.
add('RTE', '', '  mov  edi, [r15+60]\n' + KEEP('rax') + GETL +
              '  mov  [rsp], rax\n  mov  edi, [r15+60]\n  add  edi, 4\n  mov  [r15+60], edi\n'
              + GETL + '  mov  ecx, [rsp]\n  add  rsp, 16\n'
              '  mov  edx, [r15+60]\n  add  edx, 4\n  mov  [r15+60], edx\n'
              '  and  eax, 0x3F3\n  mov  ebx, eax\n  mov  eax, ecx\n')

# TRAPA #imm: push SR, push PC+2, eax = mem[VBR + imm*4]
add('TRAPA', '  mov  edx, 0\n',
    '  mov  edi, [r15+60]\n  sub  edi, 4\n  mov  [r15+60], edi\n  mov  esi, ebx\n'
    + KEEP('rdx') + SETL + DROP('rdx') +
    '  mov  edi, [r15+60]\n  sub  edi, 4\n  mov  [r15+60], edi\n  lea  esi, [r14+2]\n'
    + KEEP('rdx') + SETL + DROP('rdx') +
    '  lea  edi, [rdx*4]\n  add  edi, [r15+72]\n' + GETL, imm='1')

# --- PC-relative loads -----------------------------------------------------
# mov.w/l @(disp,PC),Rn -- disp comes from the low byte, scaled by 2 / 4.
add('MOVWI', TWON, '  lea  edi, [r14+rcx*2+4]\n' + KEEP('rdx') + GETW + DROP('rdx') +
    '  movsx eax, ax\n  mov  [r15+rdx*4], eax\n', dest='6', imm='1')
add('MOVLI', TWON, '  lea  edi, [r14+4]\n  and  edi, -4\n  lea  edi, [rdi+rcx*4]\n'
    + KEEP('rdx') + GETL + DROP('rdx') +
    '  mov  [r15+rdx*4], eax\n', dest='6', imm='1')

# --- control registers -----------------------------------------------------
# All of these take their register from bits 11-8 (the opcode mask is 0xf0ff,
# so bits 7-0 select the register and bits 15-12 the operation).
SR='[r15+64]'; GBR='[r15+68]'; VBR='[r15+72]'; MACH='[r15+76]'; MACL='[r15+80]'; PR='[r15+84]'
def stc(n, src):
    add(n, '  mov  edx, 0\n', f'  mov  eax, {src}\n  mov  [r15+rdx*4], eax\n', dest='1')
for n,src in (('STC_SR', 'ebx'), ('STC_GBR', GBR), ('STC_VBR', VBR),
              ('STS_MACH', MACH), ('STS_MACL', MACL), ('STS_PR', PR)):
    stc(n, src)

# store into @-Rn (Rn is decremented before the store)
def stcmem(n, src):
    add(n, '  mov  edx, 0\n', '  sub  dword ptr [r15+rdx*4], 4\n  mov  edi, [r15+rdx*4]\n'
        f'  mov  esi, {src}\n' + SETL, dest='1')
for n,src in (('STC_SR_MEM', 'ebx'), ('STC_GBR_MEM', GBR), ('STC_VBR_MEM', VBR),
              ('STS_MACH_DEC', MACH), ('STS_MACL_DEC', MACL), ('STSMPR', PR)):
    stcmem(n, src)

def ldx(n, dst):
    add(n, '  mov  edx, 0\n', f'  mov  eax, [r15+rdx*4]\n  mov  {dst}, eax\n', dest='1')
for n,dst in (('LDCGBR', GBR), ('LDC_VBR', VBR), ('LDS_MACH', MACH), ('LDS_MACL', MACL), ('LDS_PR', PR)):
    ldx(n, dst)
# ldc Rn,SR and ldc.l @Rn+,SR keep only the architecturally visible bits
add('LDC_SR', '  mov  edx, 0\n', '  mov  eax, [r15+rdx*4]\n  and  eax, 0x3F3\n  mov  ebx, eax\n', dest='1')

# load from @Rn+ into a control register (post-increment before the read)
def ldinc(n, dst, mask=''):
    add(n, '  mov  edx, 0\n', '  mov  edi, [r15+rdx*4]\n  add  dword ptr [r15+rdx*4], 4\n'
        + GETL + f'{mask}  mov  {dst}, eax\n', dest='1')
for n,dst in (('LDC_GBR_INC', GBR), ('LDC_VBR_INC', VBR), ('LDS_MACH_INC', MACH),
              ('LDS_MACL_INC', MACL), ('LDS_PR_INC', PR)):
    ldinc(n, dst)
ldinc('LDC_SR_INC', 'ebx', '  and  eax, 0x3F3\n')

# --- division ---------------------------------------------------------------
# DIV0S: Q = MSB(Rn), M = MSB(Rm), T = Q ^ M  (interpreter semantics).
add('DIV0S', TWON, '  mov  eax, [r15+rdx*4]\n  shr  eax, 31\n'
                   '  mov  esi, [r15+rcx*4]\n  shr  esi, 31\n'
                   '  mov  edi, eax\n  xor  edi, esi\n'
                   '  and  ebx, 0xFFFFFCFE\n  or   ebx, edi\n'
                   '  shl  eax, 8\n  or   ebx, eax\n'
                   '  shl  esi, 9\n  or   ebx, esi\n', src='1', dest='6')

# DIV1: literal transliteration of dynalib_arm64.s.  New Q = MSB(Rn) before
# the shift, Rn = (Rn << 1) | T, then one of the four paths selected by
# (old Q, M); each path stores the new Rn, computes tmp = carry/borrow and
# picks Q = (new Q == 0) ? !tmp : tmp  (or the mirror image for the "old Q != M"
# paths); finally T = (Q == M).
add('DIV1', TWON,
    '  mov  r8d, [r15+rcx*4]\n'                        # Rm
    '  mov  r9d, [r15+rdx*4]\n'                        # Rn
    '  mov  r10d, r9d\n  shr  r10d, 31\n'               # new Q = MSB(Rn)
    '  mov  eax, ebx\n  and  eax, 1\n'                  # T
    '  add  r9d, r9d\n  or   r9d, eax\n'                # Rn = (Rn << 1) | T
    '  mov  [r15+rdx*4], r9d\n'
    '  mov  r11d, ebx\n  shr  r11d, 9\n  and  r11d, 1\n'  # M
    '  mov  eax, ebx\n  shr  eax, 8\n  and  eax, 1\n'     # old Q
    '  test eax, eax\n  jz   .Ldiv1_oldq0\n'
    '  test r11d, r11d\n  jz   .Ldiv1_m0_add\n'
    # old_q=1, M=1: sub, tmp = borrow (old < new), select on (new Q == 0)
    '  mov  eax, r9d\n  sub  r9d, r8d\n  mov  [r15+rdx*4], r9d\n'
    '  cmp  eax, r9d\n  setb cl\n  jmp  .Ldiv1_sel_eq\n'
    '.Ldiv1_oldq0:\n'
    '  test r11d, r11d\n  jz   .Ldiv1_m0_sub\n'
    # old_q=0, M=1: add, tmp = carry (old > new), select on (new Q == 0)
    '  mov  eax, r9d\n  add  r9d, r8d\n  mov  [r15+rdx*4], r9d\n'
    '  cmp  eax, r9d\n  seta cl\n'
    '.Ldiv1_sel_eq:\n'
    '  movzx ecx, cl\n  mov  eax, ecx\n  xor  eax, 1\n'
    '  test r10d, r10d\n  cmovz ecx, eax\n  jmp  .Ldiv1_fin\n'
    '.Ldiv1_m0_add:\n'
    # old_q=1, M=0: add, tmp = carry, select on (new Q != 0)
    '  mov  eax, r9d\n  add  r9d, r8d\n  mov  [r15+rdx*4], r9d\n'
    '  cmp  eax, r9d\n  seta cl\n  jmp  .Ldiv1_sel_ne\n'
    '.Ldiv1_m0_sub:\n'
    # old_q=0, M=0: sub, tmp = borrow, select on (new Q != 0)
    '  mov  eax, r9d\n  sub  r9d, r8d\n  mov  [r15+rdx*4], r9d\n'
    '  cmp  eax, r9d\n  setb cl\n'
    '.Ldiv1_sel_ne:\n'
    '  movzx ecx, cl\n  mov  eax, ecx\n  xor  eax, 1\n'
    '  test r10d, r10d\n  cmovnz ecx, eax\n'
    '.Ldiv1_fin:\n'
    '  cmp  ecx, r11d\n  sete al\n  movzx eax, al\n'
    '  and  ebx, 0xFFFFFEFE\n  or   ebx, eax\n'
    '  mov  eax, ecx\n  shl  eax, 8\n  or   ebx, eax\n', src='1', dest='6')

# --- MAC: 64-bit / 32-bit accumulate with the S-bit saturation ---------------
# MAC.L @Rm+,@Rn+ (the n word is read first, matching the interpreter)
add('MAC_L', TWON,
    '  mov  edi, [r15+rdx*4]\n  add  dword ptr [r15+rdx*4], 4\n'
    '  mov  esi, [r15+rcx*4]\n  add  dword ptr [r15+rcx*4], 4\n' + KEEP('rsi') + GETL +
    DROP('rsi') + '  mov  edi, esi\n' + KEEP('rax') + GETL + DROP('r8') +
    '  movsxd rax, eax\n  movsxd r8, r8d\n  imul rax, r8\n'          # b = (s64)m0*m1
    '  mov  ecx, [r15+80]\n  mov  edx, [r15+76]\n  shl  rdx, 32\n'
    '  or   rcx, rdx\n  add  rcx, rax\n'                             # sum = MAC + b
    '  test ebx, 2\n  jz   .Lmacl_store\n'
    '  mov  rdx, 0x00007FFFFFFFFFFF\n  cmp  rcx, rdx\n  jbe  .Lmacl_store\n'
    '  mov  rdx, 0xFFFF800000000000\n  cmp  rcx, rdx\n  jae  .Lmacl_store\n'
    '  mov  rdx, 0xFFFF800000000000\n  test rax, rax\n  js   .Lmacl_set\n'
    '  mov  rdx, 0x00007FFFFFFFFFFF\n'
    '.Lmacl_set:\n  mov  rcx, rdx\n'
    '.Lmacl_store:\n'
    '  mov  [r15+80], ecx\n  shr  rcx, 32\n  mov  [r15+76], ecx\n', src='1', dest='6')

# MAC.W @Rm+,@Rn+ (the m word is read first here)
add('MAC_W', TWON,
    '  mov  edi, [r15+rcx*4]\n  add  dword ptr [r15+rcx*4], 2\n'
    '  mov  esi, [r15+rdx*4]\n  add  dword ptr [r15+rdx*4], 2\n' + KEEP('rsi') + GETW +
    DROP('rsi') + '  movsx eax, ax\n' + KEEP('rax') + '  mov  edi, esi\n' + GETW + DROP('r8') +
    '  movsx eax, ax\n  movsx r8d, r8w\n  imul eax, r8d\n  movsxd rax, eax\n'   # b
    '  mov  ecx, [r15+80]\n  movsxd rcx, ecx\n  add  rcx, rax\n'      # sum = (s32)MACL + b
    '  test ebx, 2\n  jz   .Lmacw_nosat\n'
    '  mov  rdx, 0x000000007FFFFFFF\n  cmp  rcx, rdx\n  jbe  .Lmacw_store\n'
    '  mov  rdx, 0xFFFFFFFF80000000\n  cmp  rcx, rdx\n  jae  .Lmacw_store\n'
    '  or   dword ptr [r15+76], 1\n'
    '  mov  rdx, 0x000000007FFFFFFF\n  test rax, rax\n  jns  .Lmacw_set\n'
    '  mov  rdx, 0x80000000\n'
    '.Lmacw_set:\n  mov  rcx, rdx\n  jmp  .Lmacw_store\n'
    '.Lmacw_nosat:\n'
    '  mov  eax, ecx\n  shr  rcx, 32\n  mov  [r15+76], ecx\n  mov  ecx, eax\n'
    '.Lmacw_store:\n  mov  [r15+80], ecx\n', src='1', dest='6')

add('NOP', '', '  nop\n')
add('RTS', '  mov  eax, [r15+84]\n', '')

# A typo in a name would leave the real op a ud2 stub and the dead entry would
# never be emitted, so refuse to generate anything that is not in the table.
for n in OPS:
    if n not in names:
        raise SystemExit('not an op name in %s: %s' % (AR, n))

def meta(n,size,src,dest,off1,imm,off3):
    return (f'.global {n}_size\n{n}_size: .word {size}\n'
            f'.global {n}_src\n{n}_src: .byte {src}\n'
            f'.global {n}_dest\n{n}_dest: .byte {dest}\n'
            f'.global {n}_off1\n{n}_off1: .byte {off1}\n'
            f'.global {n}_imm\n{n}_imm: .byte {imm}\n'
            f'.global {n}_off3\n{n}_off3: .byte {off3}\n')

def gen(sizes):
    body=['/* ---- generated ops: do not edit below, run gen_x64_asm.py ---- */']
    for n,(pre,b,src,dest,off1,imm,off3) in OPS.items():
        sz = sizes.get(n, 0)
        body.append(f'.global x86_{n}\nx86_{n}:\n{pre}{b}'
                    f'.size x86_{n}, .-x86_{n}\n' + meta(n, sz, src, dest, off1, imm, off3))
    stub=[]
    for n in names:
        if n in OPS: continue
        stub.append(f'.global x86_{n}\nx86_{n}:\n  ud2\n.size x86_{n}, .-x86_{n}\n' + meta(n,2,'0xff','0xff','0xff','0xff','0xff'))
    return HEAD + '\n'.join(body) + '\n/* ---- not implemented yet: trap so a wrong block is visible ---- */\n' + '\n'.join(stub) + '\n' + TAIL

def measure(path='/tmp/genmeasure.o', all_syms=False):
    subprocess.run(['gcc','-c',OUT,'-o',path],check=True)
    out=subprocess.run(['nm','-S',path],capture_output=True,text=True).stdout
    sz={}
    for line in out.split('\n'):
        p=line.split()
        if len(p) != 4: continue
        if all_syms:
            sz[p[3]]=int(p[1],16)
        elif p[3].startswith('x86_'):
            sz[p[3][4:]]=int(p[1],16)
    return sz

# Structural fragments: the emitter charges these sizes and copies exactly that
# many bytes into a block, so a drift between the .S and the header corrupts
# code at run time -- silently.  GAS cannot assert it itself (a branch under
# relaxation makes `. - sym` non-constant inside .if), hence the check here,
# where the file is assembled anyway.  The header stays the single source of
# truth for the numbers.
STRUCT={'prologue':'PROLOGSIZE','epilogue':'EPILOGSIZE','epilogue_k':'EPILOGSIZE',
        'PageFlip':'DELAYJUMPSIZE','PageFlip_k':'DELAYJUMPSIZE',
        'seperator_normal':'SEPERATORSIZE_NORMAL','seperator_d_normal':'SEPERATORSIZE_DEBUG',
        'seperator_d_delay':'SEPERATORSIZE_DELAYD_DEBUG',
        'seperator_delay_slot':'SEPERATORSIZE_DELAY_SLOT',
        'seperator_delay_slot_k':'SEPERATORSIZE_DELAY_SLOT',
        'seperator_delay_after':'SEPERATORSIZE_DELAY_AFTER',
        'seperator_delay_after_k':'SEPERATORSIZE_DELAY_AFTER',
        'internal_jmp':'DYN_INTERNAL_JMP_SIZE',
        'internal_delay_jmp':'DYN_INTERNAL_DELAY_JMP_SIZE'}

def check_struct(sizes):
    defs={k:int(v) for k,v in re.findall(r'#define\s+([A-Z0-9_]+)\s+(\d+)', open(HDR).read())}
    bad=[]
    for sym,macro in STRUCT.items():
        want=defs.get(macro)
        if want is None or sizes.get(sym) != want:
            bad.append('%-24s %s bytes, %s = %s' % (sym, sizes.get(sym), macro, want))
    if bad:
        raise SystemExit('struct fragment size mismatch:\n  ' + '\n  '.join(bad))
    print('struct fragments ok:', ', '.join('%s=%d' % (s_, defs[STRUCT[s_]])
          for s_ in ('prologue','epilogue_k','PageFlip_k','seperator_delay_slot_k',
                     'seperator_delay_after_k','internal_jmp')))

open(OUT,'w').write(gen({}))
sizes=measure()
open(OUT,'w').write(gen(sizes))
sizes2=measure()
check_struct(measure(all_syms=True))
print('ops:', len(OPS), 'sizes stable:', sizes==sizes2)
print({k:sizes2[k] for k in sorted(OPS)})
