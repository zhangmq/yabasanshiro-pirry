/*
        Copyright 2019 devMiyax(smiyaxdev@gmail.com)

This file is part of YabaSanshiro.

        YabaSanshiro is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

YabaSanshiro is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

        You should have received a copy of the GNU General Public License
along with YabaSanshiro; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA

; mov	16	レジスタ間のコピー
; ldr	13	メモリからレジスタに読み出し
; b	10	無条件分岐
; bl	9	サブルーチン呼び出し
; add	8	加算
; cmp	6	比較
; str	6	レジスタからメモリへ格納
; ldp	5	スタックから取り出し
; stp	4	スタックへ格納
; adrp	3	レジスタにアドレスを設定
; cbz	3	比較して 0 なら分岐
; b.eq	3	等しければ分岐
; ret	3	サブルーチンから戻る
; sub	2	減算
; b.ne	2	異なれば分岐
; adr	1	レジスタにアドレスを設定
; cbnz	1	比較して非 0 なら分岐
; and	1	ビット積

; w0  ; not keeped on function call
; w1  ; not keeped on function call  
; w2  ; not keeped on function call
; w3  ; not keeped on function call
; w4  
; w5  
; w6


; w19   <- Address of GenReg
; w20   <- PC
; w21   <- ClockCounter

; w22 - 28 safe from fucntion call

; w10  <- 
; w11     ; scrach
; w12 ip
; w13 sp
; w14 lr  ; scrach
; w15 pc

  u32 GenReg[16];
  u32 CtrlReg[3];
  u32 SysReg[6];

  MACH   [x19, #(16+3+0)*4 ]  
  MACL   [x19, #(16+3+1)*4 ]  
  PR     [x19, #(16+3+2)*4 ]  
  PC     [x19, #(16+3+3)*4 ]  
  COUNT  [x19, #(16+3+4)*4 ]  
  ICOUNT [x19, #(16+3+5)*4 ]  
  SR     [x19, #(16+0)*4 ]  
  GBR     [x19, #(16+1)*4 ]  
  VBR     [x19, #(16+2)*4 ]  
  getmembyte = [x19, #(16+3+6+0)*8 ] ;
  getmemword = [x19, #(16+3+6+1)*8 ] ;
  getmemlong = [x19, #(16+3+6+2)*8 ] ;
  setmembyte = [x19, #(16+3+6+3)*8 ] ;
  setmemword = [x19, #(16+3+6+4)*8 ] ;
  setmemlong = [x19, #(16+3+6+5)*8 ] ;

  http://www.mztn.org/slasm/arm07.html
  http://www.peter-cockerell.net/aalp/html/ch-3.html

  union {
    struct {
      u32 T:1;
      u32 S:1;
      u32 reserved0:2;
      u32 I:4;
      u32 Q:1;
      u32 M:1;
      u32 reserved1:22;
    } part;
    u32 all;
  } SR;
 
*/

/*
   x86-64 helper library (SysV, Linux) -- work in progress.

   Counterpart of dynalib_arm64.s: the emitter copies these routines into a
   block, patches the operand fields and jumps in, so a snippet is a fragment,
   not a function.  Only prologue/epilogue save and restore the block state.

   Register state inside a block (callee-saved: a snippet may call C):

     r15  tagSH2 pointer    (AArch64 x19)      eax  scratch / branch target
     r14d PC                (w20)              ecx, edx, esi, edi, r8-r11 scratch
     r13d COUNT             (w21)
     r12d exitcount         (w25)
     ebx  SR                (w26)

   The five pushes in the prologue leave rsp 16-byte aligned.

   Operand patching: register operands are SIB indices (scale 4), so the
   patched value is the SH-2 register number and the metadata below is the byte
   offset of that imm32 inside the snippet.

   Ops that are not implemented yet are assembled as ud2 with size 2 and no
   operand fields: a block that reaches one traps instead of running wild.
*/

.intel_syntax noprefix
.section .text


.global prologue
prologue:
  push rbx
  push r12
  push r13
  push r14
  push r15
  mov  r15, rdi
  mov  r14d, [r15+88]
  mov  r13d, [r15+92]
  mov  r12d, [r15+160]
  mov  ebx,  [r15+64]
.size prologue, .-prologue

.global epilogue
epilogue:
  mov  [r15+88], r14d
  mov  [r15+92], r13d
  mov  [r15+64], ebx
  pop  r15
  pop  r14
  pop  r13
  pop  r12
  pop  rbx
  ret
.size epilogue, .-epilogue

/* The chained exits are aliases for now: falling back to C instead of taking
   the in-JIT link is only slower, and it keeps the sizes the emitter copies. */
/* ---- block chaining -------------------------------------------------------
   The _k variants are the non-debug block exits.  Instead of returning to C
   they jump through [r15+216] (chain_target, set to chain_stub by
   SetCurrentContext); chain_stub enters the next HWRAM block behind its
   prologue when C would do the same, and falls back to the ordinary epilogue
   otherwise.  Every fragment has exactly the size the emitter charges
   (EPILOGSIZE / DELAYJUMPSIZE / SEPERATORSIZE_*), so block layouts do not
   change; the .if checks below turn a size drift into a build error.
   State on entry: r15 tagSH2, r14d PC, r13d COUNT, r12d exitcount, ebx SR,
   and the 40-byte frame of the prologue. */
.global epilogue_k
epilogue_k:
  jmp  qword ptr [r15+216]
  .fill 15, 1, 0x90
.size epilogue_k, .-epilogue_k

.size epilogue_k, .-epilogue_k

/* exit with PC = the value in eax (0 = keep the current PC) */
.global PageFlip
PageFlip:
  test eax, eax
  jz   1f
  mov  [r15+88], eax
  mov  [r15+92], r13d
  mov  [r15+64], ebx
  pop  r15
  pop  r14
  pop  r13
  pop  r12
  pop  rbx
  ret
1:
.size PageFlip, .-PageFlip

/* Branch exits (BT/BF/TRAPA): eax = target, 0 = fall through to the next
   instruction, which was emitted right behind this fragment. */
.global PageFlip_k
PageFlip_k:
  test eax, eax
  jz   1f
  mov  r14d, eax
  jmp  qword ptr [r15+216]
1:
  .fill 12, 1, 0x90
.size PageFlip_k, .-PageFlip_k

/* block separator: PC += 2, COUNT += <cycle patched at +7> */
.global seperator_normal
seperator_normal:
  add  r14d, 2
  add  r13d, 1
.size seperator_normal, .-seperator_normal

/* debug build only (SH2DynDebug): after every instruction run one interpreter
   step through tagSH2->eachclock (DebugEachClock) and end the block when it
   returns 1.  Same shape as the AArch64 seperator_d_normal. */
.global seperator_d_normal
seperator_d_normal:
  add  r14d, 2
  add  r13d, 1
  mov  [r15+88], r14d
  mov  [r15+92], r13d
  mov  [r15+64], ebx
  push rax
  push rax
  call qword ptr [r15+152]
  add  rsp, 16
  cmp  eax, 1
  jne  .Lsd_cont
  pop  r15
  pop  r14
  pop  r13
  pop  r12
  pop  rbx
  ret
.Lsd_cont:
  /* The AArch64 fragment keeps its last instruction (the branch-decision
     restore) outside SEPERATORSIZE_DEBUG, so the continue path loses eax and
     the next instruction sees eachclock's return value.  Reproduced here on
     purpose: no restore. */
.size seperator_d_normal, .-seperator_d_normal

.global seperator_delay_slot
seperator_delay_slot:
  test eax, eax
  jne  .Ldsl_cont
  add  r14d, 2
  mov  [r15+88], r14d
  add  r13d, 1
  mov  [r15+92], r13d
  mov  [r15+64], ebx
  pop  r15
  pop  r14
  pop  r13
  pop  r12
  pop  rbx
  ret
.Ldsl_cont:
  mov  r14d, eax
  sub  r14d, 2
.size seperator_delay_slot, .-seperator_delay_slot

/* After the branch snippet, before the delay-slot instruction.
   eax == 0 (not taken): PC = delay slot, count it, chain there.
   eax != 0 (taken):     PC = target - 2 so the delay slot runs next. */
.global seperator_delay_slot_k
seperator_delay_slot_k:
  test eax, eax
  jne  .Ldslk_taken
  add  r14d, 2
  add  r13d, 1
  jmp  qword ptr [r15+216]
.Ldslk_taken:
  mov  r14d, eax
  sub  r14d, 2
  .fill 15, 1, 0x90
.size seperator_delay_slot_k, .-seperator_delay_slot_k

.size seperator_delay_slot_k, .-seperator_delay_slot_k

/* after the delay slot: the branch target is in PC-2, store and leave */
.global seperator_delay_after
seperator_delay_after:
  add  r14d, 2
  mov  [r15+88], r14d
  add  r13d, 1
  mov  [r15+92], r13d
  mov  [r15+64], ebx
  pop  r15
  pop  r14
  pop  r13
  pop  r12
  pop  rbx
  /* AArch64's debug delay-after patches `add sp, sp, #0x50` at offset 40
     (DALAY_CLOCK_OFFSET_DEBUG) instead of its cycle slot at offset 8, so the
     block returns with the caller's stack pointer 0x50|cycle - 0x50 bytes too
     high.  Same shape here: return to the right address but leak `cycle`
     bytes of stack.  The imm8 of `add rsp` is at offset 44. */
  .fill 11, 1, 0x90
  pop  rax
  add  rsp, 0
  jmp  rax
.size seperator_delay_after, .-seperator_delay_after

/* After the delay slot: PC = branch target.  The cycle count is patched into
   the imm8 of `add r13d` at offset 11 (DALAY_CLOCK_OFFSET), so keep the four
   bytes before it fixed. */
.global seperator_delay_after_k
seperator_delay_after_k:
  add  r14d, 2
  mov  [r15+88], r14d
  add  r13d, 1
  jmp  qword ptr [r15+216]
  .fill 28, 1, 0x90
.size seperator_delay_after_k, .-seperator_delay_after_k

/* debug-build delay separator (AArch64 seperator_d_delay): store the state,
   run one interpreter step, then either leave the block (not taken: PC at the
   delay slot) or continue into the delay-slot instruction (taken: PC =
   target-2).  The branch decision is saved across the call. */
.global seperator_d_delay
seperator_d_delay:
  push rax
  push rax
  mov  [r15+88], r14d
  mov  [r15+92], r13d
  mov  [r15+64], ebx
  call qword ptr [r15+152]
  mov  ecx, [rsp]
  add  rsp, 16
  test ecx, ecx
  jnz  .Lsdd_cont
  add  r14d, 2
  mov  [r15+88], r14d
  add  r13d, 1
  mov  [r15+92], r13d
  pop  r15
  pop  r14
  pop  r13
  pop  r12
  pop  rbx
  ret
.Lsdd_cont:
  mov  r14d, ecx
  /* AArch64's `sub w20, w20, #2` sits outside SEPERATORSIZE_DELAYD_DEBUG:
     the taken path leaves PC at the branch target, not target-2. */
.size seperator_d_delay, .-seperator_d_delay

/* block-internal jump (AArch64 internal_jmp, same control flow):
     COUNT > exitcount            -> leave the block (PC = branch target if
                                     taken, current PC otherwise)
     branch not taken             -> fall through to the next instruction
     branch taken                 -> PC = target, jump to the target snippet
                                     inside this block (rel32 patched by the
                                     emitter at +<DYN_INTERNAL_JMP_TO_OFFSET>)
   The target code is already part of this block (addr_map hit), so this never
   leaves the block on the taken path. */
.global internal_jmp
internal_jmp:
  cmp  r13d, r12d
  ja   .Lij_exit
  test eax, eax
  jz   .Lij_next
  mov  r14d, eax
  .byte 0xe9
  .long 0                  /* patched rel32: target snippet in this block */
.Lij_patched:
.Lij_exit:
  test eax, eax
  jz   1f
  mov  [r15+88], eax
  jmp  2f
1:
  mov  [r15+88], r14d
2:
  mov  [r15+92], r13d
  mov  [r15+64], ebx
  pop  r15
  pop  r14
  pop  r13
  pop  r12
  pop  rbx
  ret
.Lij_next:
.size internal_jmp, .-internal_jmp

/* block-internal jump after a delayed branch (AArch64 internal_delay_jmp):
   only the taken path reaches this fragment -- the delay-slot separator
   already left the block when the branch was not taken.
     PC += 2 ; COUNT += cycle (imm patched at DALAY_CLOCK_OFFSET)
     pre-COUNT > exitcount -> leave the block, else jump to the target
   COUNT is written back on both paths, like the AArch64 fragment. */
.global internal_delay_jmp
internal_delay_jmp:
  add  r14d, 2
  mov  ecx, r13d
  nop
  add  r13d, 1
  cmp  ecx, r12d
  ja   .Lidj_exit
  .byte 0xe9
  .long 0                  /* patched rel32: target snippet in this block */
.Lidj_patched:
.Lidj_exit:
  mov  [r15+88], r14d
  mov  [r15+92], r13d
  mov  [r15+64], ebx
  pop  r15
  pop  r14
  pop  r13
  pop  r12
  pop  rbx
  ret
.size internal_delay_jmp, .-internal_delay_jmp


/* add Rm,Rn : Rn += Rm; the two mov immediates are the patched registers */
/* ---- generated ops: do not edit below, run gen_x64_asm.py ---- */
.global x86_AND
x86_AND:
  mov  ecx, 0
  mov  edx, 0
  mov  eax, [r15+rcx*4]
  and  dword ptr [r15+rdx*4], eax
.size x86_AND, .-x86_AND
.global AND_size
AND_size: .word 18
.global AND_src
AND_src: .byte 1
.global AND_dest
AND_dest: .byte 6
.global AND_off1
AND_off1: .byte 0xff
.global AND_imm
AND_imm: .byte 0xff
.global AND_off3
AND_off3: .byte 0xff

.global x86_OR
x86_OR:
  mov  ecx, 0
  mov  edx, 0
  mov  eax, [r15+rcx*4]
  or   dword ptr [r15+rdx*4], eax
.size x86_OR, .-x86_OR
.global OR_size
OR_size: .word 18
.global OR_src
OR_src: .byte 1
.global OR_dest
OR_dest: .byte 6
.global OR_off1
OR_off1: .byte 0xff
.global OR_imm
OR_imm: .byte 0xff
.global OR_off3
OR_off3: .byte 0xff

.global x86_XOR
x86_XOR:
  mov  ecx, 0
  mov  edx, 0
  mov  eax, [r15+rcx*4]
  xor  dword ptr [r15+rdx*4], eax
.size x86_XOR, .-x86_XOR
.global XOR_size
XOR_size: .word 18
.global XOR_src
XOR_src: .byte 1
.global XOR_dest
XOR_dest: .byte 6
.global XOR_off1
XOR_off1: .byte 0xff
.global XOR_imm
XOR_imm: .byte 0xff
.global XOR_off3
XOR_off3: .byte 0xff

.global x86_SUB
x86_SUB:
  mov  ecx, 0
  mov  edx, 0
  mov  eax, [r15+rcx*4]
  sub  dword ptr [r15+rdx*4], eax
.size x86_SUB, .-x86_SUB
.global SUB_size
SUB_size: .word 18
.global SUB_src
SUB_src: .byte 1
.global SUB_dest
SUB_dest: .byte 6
.global SUB_off1
SUB_off1: .byte 0xff
.global SUB_imm
SUB_imm: .byte 0xff
.global SUB_off3
SUB_off3: .byte 0xff

.global x86_TST
x86_TST:
  mov  ecx, 0
  mov  edx, 0
  mov  eax, [r15+rcx*4]
  test eax, dword ptr [r15+rdx*4]
  sete al
  movzx eax, al
  and  ebx, -2
  or   ebx, eax
.size x86_TST, .-x86_TST
.global TST_size
TST_size: .word 29
.global TST_src
TST_src: .byte 1
.global TST_dest
TST_dest: .byte 6
.global TST_off1
TST_off1: .byte 0xff
.global TST_imm
TST_imm: .byte 0xff
.global TST_off3
TST_off3: .byte 0xff

.global x86_CMP_EQ
x86_CMP_EQ:
  mov  ecx, 0
  mov  edx, 0
  mov  eax, [r15+rcx*4]
  cmp dword ptr [r15+rdx*4], eax
  sete al
  movzx eax, al
  and  ebx, -2
  or   ebx, eax
.size x86_CMP_EQ, .-x86_CMP_EQ
.global CMP_EQ_size
CMP_EQ_size: .word 29
.global CMP_EQ_src
CMP_EQ_src: .byte 1
.global CMP_EQ_dest
CMP_EQ_dest: .byte 6
.global CMP_EQ_off1
CMP_EQ_off1: .byte 0xff
.global CMP_EQ_imm
CMP_EQ_imm: .byte 0xff
.global CMP_EQ_off3
CMP_EQ_off3: .byte 0xff

.global x86_CMP_HS
x86_CMP_HS:
  mov  ecx, 0
  mov  edx, 0
  mov  eax, [r15+rcx*4]
  cmp dword ptr [r15+rdx*4], eax
  setae al
  movzx eax, al
  and  ebx, -2
  or   ebx, eax
.size x86_CMP_HS, .-x86_CMP_HS
.global CMP_HS_size
CMP_HS_size: .word 29
.global CMP_HS_src
CMP_HS_src: .byte 1
.global CMP_HS_dest
CMP_HS_dest: .byte 6
.global CMP_HS_off1
CMP_HS_off1: .byte 0xff
.global CMP_HS_imm
CMP_HS_imm: .byte 0xff
.global CMP_HS_off3
CMP_HS_off3: .byte 0xff

.global x86_CMP_HI
x86_CMP_HI:
  mov  ecx, 0
  mov  edx, 0
  mov  eax, [r15+rcx*4]
  cmp dword ptr [r15+rdx*4], eax
  seta al
  movzx eax, al
  and  ebx, -2
  or   ebx, eax
.size x86_CMP_HI, .-x86_CMP_HI
.global CMP_HI_size
CMP_HI_size: .word 29
.global CMP_HI_src
CMP_HI_src: .byte 1
.global CMP_HI_dest
CMP_HI_dest: .byte 6
.global CMP_HI_off1
CMP_HI_off1: .byte 0xff
.global CMP_HI_imm
CMP_HI_imm: .byte 0xff
.global CMP_HI_off3
CMP_HI_off3: .byte 0xff

.global x86_CMP_GT
x86_CMP_GT:
  mov  ecx, 0
  mov  edx, 0
  mov  eax, [r15+rcx*4]
  cmp dword ptr [r15+rdx*4], eax
  setg al
  movzx eax, al
  and  ebx, -2
  or   ebx, eax
.size x86_CMP_GT, .-x86_CMP_GT
.global CMP_GT_size
CMP_GT_size: .word 29
.global CMP_GT_src
CMP_GT_src: .byte 1
.global CMP_GT_dest
CMP_GT_dest: .byte 6
.global CMP_GT_off1
CMP_GT_off1: .byte 0xff
.global CMP_GT_imm
CMP_GT_imm: .byte 0xff
.global CMP_GT_off3
CMP_GT_off3: .byte 0xff

.global x86_CMP_GE
x86_CMP_GE:
  mov  ecx, 0
  mov  edx, 0
  mov  eax, [r15+rcx*4]
  cmp dword ptr [r15+rdx*4], eax
  setge al
  movzx eax, al
  and  ebx, -2
  or   ebx, eax
.size x86_CMP_GE, .-x86_CMP_GE
.global CMP_GE_size
CMP_GE_size: .word 29
.global CMP_GE_src
CMP_GE_src: .byte 1
.global CMP_GE_dest
CMP_GE_dest: .byte 6
.global CMP_GE_off1
CMP_GE_off1: .byte 0xff
.global CMP_GE_imm
CMP_GE_imm: .byte 0xff
.global CMP_GE_off3
CMP_GE_off3: .byte 0xff

.global x86_NEG
x86_NEG:
  mov  ecx, 0
  mov  edx, 0
  mov  eax, [r15+rcx*4]
  neg  eax
  mov  [r15+rdx*4], eax
.size x86_NEG, .-x86_NEG
.global NEG_size
NEG_size: .word 20
.global NEG_src
NEG_src: .byte 1
.global NEG_dest
NEG_dest: .byte 6
.global NEG_off1
NEG_off1: .byte 0xff
.global NEG_imm
NEG_imm: .byte 0xff
.global NEG_off3
NEG_off3: .byte 0xff

.global x86_NOT
x86_NOT:
  mov  ecx, 0
  mov  edx, 0
  mov  eax, [r15+rcx*4]
  not  eax
  mov  [r15+rdx*4], eax
.size x86_NOT, .-x86_NOT
.global NOT_size
NOT_size: .word 20
.global NOT_src
NOT_src: .byte 1
.global NOT_dest
NOT_dest: .byte 6
.global NOT_off1
NOT_off1: .byte 0xff
.global NOT_imm
NOT_imm: .byte 0xff
.global NOT_off3
NOT_off3: .byte 0xff

.global x86_SWAP_W
x86_SWAP_W:
  mov  ecx, 0
  mov  edx, 0
  mov  eax, [r15+rcx*4]
  rol  eax, 16
  mov  [r15+rdx*4], eax
.size x86_SWAP_W, .-x86_SWAP_W
.global SWAP_W_size
SWAP_W_size: .word 21
.global SWAP_W_src
SWAP_W_src: .byte 1
.global SWAP_W_dest
SWAP_W_dest: .byte 6
.global SWAP_W_off1
SWAP_W_off1: .byte 0xff
.global SWAP_W_imm
SWAP_W_imm: .byte 0xff
.global SWAP_W_off3
SWAP_W_off3: .byte 0xff

.global x86_SWAP_B
x86_SWAP_B:
  mov  ecx, 0
  mov  edx, 0
  mov  eax, [r15+rcx*4]
  xchg al, ah
  mov  [r15+rdx*4], eax
.size x86_SWAP_B, .-x86_SWAP_B
.global SWAP_B_size
SWAP_B_size: .word 20
.global SWAP_B_src
SWAP_B_src: .byte 1
.global SWAP_B_dest
SWAP_B_dest: .byte 6
.global SWAP_B_off1
SWAP_B_off1: .byte 0xff
.global SWAP_B_imm
SWAP_B_imm: .byte 0xff
.global SWAP_B_off3
SWAP_B_off3: .byte 0xff

.global x86_EXTUB
x86_EXTUB:
  mov  ecx, 0
  mov  edx, 0
  mov  eax, [r15+rcx*4]
  movzx eax, byte ptr [r15+rcx*4]
  mov  [r15+rdx*4], eax
.size x86_EXTUB, .-x86_EXTUB
.global EXTUB_size
EXTUB_size: .word 23
.global EXTUB_src
EXTUB_src: .byte 1
.global EXTUB_dest
EXTUB_dest: .byte 6
.global EXTUB_off1
EXTUB_off1: .byte 0xff
.global EXTUB_imm
EXTUB_imm: .byte 0xff
.global EXTUB_off3
EXTUB_off3: .byte 0xff

.global x86_EXTS_B
x86_EXTS_B:
  mov  ecx, 0
  mov  edx, 0
  mov  eax, [r15+rcx*4]
  movsx eax, byte ptr [r15+rcx*4]
  mov  [r15+rdx*4], eax
.size x86_EXTS_B, .-x86_EXTS_B
.global EXTS_B_size
EXTS_B_size: .word 23
.global EXTS_B_src
EXTS_B_src: .byte 1
.global EXTS_B_dest
EXTS_B_dest: .byte 6
.global EXTS_B_off1
EXTS_B_off1: .byte 0xff
.global EXTS_B_imm
EXTS_B_imm: .byte 0xff
.global EXTS_B_off3
EXTS_B_off3: .byte 0xff

.global x86_EXTU_W
x86_EXTU_W:
  mov  ecx, 0
  mov  edx, 0
  mov  eax, [r15+rcx*4]
  movzx eax, word ptr [r15+rcx*4]
  mov  [r15+rdx*4], eax
.size x86_EXTU_W, .-x86_EXTU_W
.global EXTU_W_size
EXTU_W_size: .word 23
.global EXTU_W_src
EXTU_W_src: .byte 1
.global EXTU_W_dest
EXTU_W_dest: .byte 6
.global EXTU_W_off1
EXTU_W_off1: .byte 0xff
.global EXTU_W_imm
EXTU_W_imm: .byte 0xff
.global EXTU_W_off3
EXTU_W_off3: .byte 0xff

.global x86_EXTS_W
x86_EXTS_W:
  mov  ecx, 0
  mov  edx, 0
  mov  eax, [r15+rcx*4]
  movsx eax, word ptr [r15+rcx*4]
  mov  [r15+rdx*4], eax
.size x86_EXTS_W, .-x86_EXTS_W
.global EXTS_W_size
EXTS_W_size: .word 23
.global EXTS_W_src
EXTS_W_src: .byte 1
.global EXTS_W_dest
EXTS_W_dest: .byte 6
.global EXTS_W_off1
EXTS_W_off1: .byte 0xff
.global EXTS_W_imm
EXTS_W_imm: .byte 0xff
.global EXTS_W_off3
EXTS_W_off3: .byte 0xff

.global x86_SHL
x86_SHL:
  mov  ecx, 0
  mov  eax, [r15+rcx*4]
  shl eax, 1
  setc dl
  mov  [r15+rcx*4], eax
  movzx edx, dl
  and  ebx, -2
  or   ebx, edx
.size x86_SHL, .-x86_SHL
.global SHL_size
SHL_size: .word 26
.global SHL_src
SHL_src: .byte 0xff
.global SHL_dest
SHL_dest: .byte 1
.global SHL_off1
SHL_off1: .byte 0xff
.global SHL_imm
SHL_imm: .byte 0xff
.global SHL_off3
SHL_off3: .byte 0xff

.global x86_SHLR
x86_SHLR:
  mov  ecx, 0
  mov  eax, [r15+rcx*4]
  shr eax, 1
  setc dl
  mov  [r15+rcx*4], eax
  movzx edx, dl
  and  ebx, -2
  or   ebx, edx
.size x86_SHLR, .-x86_SHLR
.global SHLR_size
SHLR_size: .word 26
.global SHLR_src
SHLR_src: .byte 0xff
.global SHLR_dest
SHLR_dest: .byte 1
.global SHLR_off1
SHLR_off1: .byte 0xff
.global SHLR_imm
SHLR_imm: .byte 0xff
.global SHLR_off3
SHLR_off3: .byte 0xff

.global x86_SHAR
x86_SHAR:
  mov  ecx, 0
  mov  eax, [r15+rcx*4]
  sar eax, 1
  setc dl
  mov  [r15+rcx*4], eax
  movzx edx, dl
  and  ebx, -2
  or   ebx, edx
.size x86_SHAR, .-x86_SHAR
.global SHAR_size
SHAR_size: .word 26
.global SHAR_src
SHAR_src: .byte 0xff
.global SHAR_dest
SHAR_dest: .byte 1
.global SHAR_off1
SHAR_off1: .byte 0xff
.global SHAR_imm
SHAR_imm: .byte 0xff
.global SHAR_off3
SHAR_off3: .byte 0xff

.global x86_ROTL
x86_ROTL:
  mov  ecx, 0
  mov  eax, [r15+rcx*4]
  rol eax, 1
  setc dl
  mov  [r15+rcx*4], eax
  movzx edx, dl
  and  ebx, -2
  or   ebx, edx
.size x86_ROTL, .-x86_ROTL
.global ROTL_size
ROTL_size: .word 26
.global ROTL_src
ROTL_src: .byte 0xff
.global ROTL_dest
ROTL_dest: .byte 1
.global ROTL_off1
ROTL_off1: .byte 0xff
.global ROTL_imm
ROTL_imm: .byte 0xff
.global ROTL_off3
ROTL_off3: .byte 0xff

.global x86_ROTR
x86_ROTR:
  mov  ecx, 0
  mov  eax, [r15+rcx*4]
  ror eax, 1
  setc dl
  mov  [r15+rcx*4], eax
  movzx edx, dl
  and  ebx, -2
  or   ebx, edx
.size x86_ROTR, .-x86_ROTR
.global ROTR_size
ROTR_size: .word 26
.global ROTR_src
ROTR_src: .byte 0xff
.global ROTR_dest
ROTR_dest: .byte 1
.global ROTR_off1
ROTR_off1: .byte 0xff
.global ROTR_imm
ROTR_imm: .byte 0xff
.global ROTR_off3
ROTR_off3: .byte 0xff

.global x86_DT
x86_DT:
  mov  ecx, 0
  sub  dword ptr [r15+rcx*4], 1
  sete al
  movzx eax, al
  and  ebx, -2
  or   ebx, eax
.size x86_DT, .-x86_DT
.global DT_size
DT_size: .word 21
.global DT_src
DT_src: .byte 0xff
.global DT_dest
DT_dest: .byte 1
.global DT_off1
DT_off1: .byte 0xff
.global DT_imm
DT_imm: .byte 0xff
.global DT_off3
DT_off3: .byte 0xff

.global x86_ADD
x86_ADD:
  mov  ecx, 0
  mov  edx, 0
  mov  eax, [r15+rcx*4]
  add  dword ptr [r15+rdx*4], eax
.size x86_ADD, .-x86_ADD
.global ADD_size
ADD_size: .word 18
.global ADD_src
ADD_src: .byte 1
.global ADD_dest
ADD_dest: .byte 6
.global ADD_off1
ADD_off1: .byte 0xff
.global ADD_imm
ADD_imm: .byte 0xff
.global ADD_off3
ADD_off3: .byte 0xff

.global x86_ADDC
x86_ADDC:
  mov  ecx, 0
  mov  edx, 0
  mov  eax, [r15+rcx*4]
  bt   ebx, 0
  adc  eax, [r15+rdx*4]
  mov  [r15+rdx*4], eax
  setc al
  movzx eax, al
  and  ebx, -2
  or   ebx, eax
.size x86_ADDC, .-x86_ADDC
.global ADDC_size
ADDC_size: .word 37
.global ADDC_src
ADDC_src: .byte 1
.global ADDC_dest
ADDC_dest: .byte 6
.global ADDC_off1
ADDC_off1: .byte 0xff
.global ADDC_imm
ADDC_imm: .byte 0xff
.global ADDC_off3
ADDC_off3: .byte 0xff

.global x86_ADDV
x86_ADDV:
  mov  ecx, 0
  mov  edx, 0
  mov  eax, [r15+rcx*4]
  add  eax, [r15+rdx*4]
  mov  [r15+rdx*4], eax
  seto al
  movzx eax, al
  and  ebx, -2
  or   ebx, eax
.size x86_ADDV, .-x86_ADDV
.global ADDV_size
ADDV_size: .word 33
.global ADDV_src
ADDV_src: .byte 1
.global ADDV_dest
ADDV_dest: .byte 6
.global ADDV_off1
ADDV_off1: .byte 0xff
.global ADDV_imm
ADDV_imm: .byte 0xff
.global ADDV_off3
ADDV_off3: .byte 0xff

.global x86_SUBC
x86_SUBC:
  mov  ecx, 0
  mov  edx, 0
  mov  eax, [r15+rcx*4]
  bt   ebx, 0
  sbb  dword ptr [r15+rdx*4], eax
  setc al
  movzx eax, al
  and  ebx, -2
  or   ebx, eax
.size x86_SUBC, .-x86_SUBC
.global SUBC_size
SUBC_size: .word 33
.global SUBC_src
SUBC_src: .byte 1
.global SUBC_dest
SUBC_dest: .byte 6
.global SUBC_off1
SUBC_off1: .byte 0xff
.global SUBC_imm
SUBC_imm: .byte 0xff
.global SUBC_off3
SUBC_off3: .byte 0xff

.global x86_SUBV
x86_SUBV:
  mov  ecx, 0
  mov  edx, 0
  mov  eax, [r15+rcx*4]
  sub  dword ptr [r15+rdx*4], eax
  seto al
  movzx eax, al
  and  ebx, -2
  or   ebx, eax
.size x86_SUBV, .-x86_SUBV
.global SUBV_size
SUBV_size: .word 29
.global SUBV_src
SUBV_src: .byte 1
.global SUBV_dest
SUBV_dest: .byte 6
.global SUBV_off1
SUBV_off1: .byte 0xff
.global SUBV_imm
SUBV_imm: .byte 0xff
.global SUBV_off3
SUBV_off3: .byte 0xff

.global x86_ADDI
x86_ADDI:
  mov  edx, 0
  mov  eax, 0
  movsx eax, al
  add  dword ptr [r15+rdx*4], eax
.size x86_ADDI, .-x86_ADDI
.global ADDI_size
ADDI_size: .word 17
.global ADDI_src
ADDI_src: .byte 0xff
.global ADDI_dest
ADDI_dest: .byte 1
.global ADDI_off1
ADDI_off1: .byte 0xff
.global ADDI_imm
ADDI_imm: .byte 6
.global ADDI_off3
ADDI_off3: .byte 0xff

.global x86_ANDI
x86_ANDI:
  mov  eax, 0
  and  dword ptr [r15], eax
.size x86_ANDI, .-x86_ANDI
.global ANDI_size
ANDI_size: .word 8
.global ANDI_src
ANDI_src: .byte 0xff
.global ANDI_dest
ANDI_dest: .byte 0xff
.global ANDI_off1
ANDI_off1: .byte 0xff
.global ANDI_imm
ANDI_imm: .byte 1
.global ANDI_off3
ANDI_off3: .byte 0xff

.global x86_ORI
x86_ORI:
  mov  eax, 0
  or   dword ptr [r15], eax
.size x86_ORI, .-x86_ORI
.global ORI_size
ORI_size: .word 8
.global ORI_src
ORI_src: .byte 0xff
.global ORI_dest
ORI_dest: .byte 0xff
.global ORI_off1
ORI_off1: .byte 0xff
.global ORI_imm
ORI_imm: .byte 1
.global ORI_off3
ORI_off3: .byte 0xff

.global x86_XORI
x86_XORI:
  mov  eax, 0
  xor  dword ptr [r15], eax
.size x86_XORI, .-x86_XORI
.global XORI_size
XORI_size: .word 8
.global XORI_src
XORI_src: .byte 0xff
.global XORI_dest
XORI_dest: .byte 0xff
.global XORI_off1
XORI_off1: .byte 0xff
.global XORI_imm
XORI_imm: .byte 1
.global XORI_off3
XORI_off3: .byte 0xff

.global x86_TSTI
x86_TSTI:
  mov  eax, 0
  test eax, dword ptr [r15]
  sete al
  movzx eax, al
  and  ebx, -2
  or   ebx, eax
.size x86_TSTI, .-x86_TSTI
.global TSTI_size
TSTI_size: .word 19
.global TSTI_src
TSTI_src: .byte 0xff
.global TSTI_dest
TSTI_dest: .byte 0xff
.global TSTI_off1
TSTI_off1: .byte 0xff
.global TSTI_imm
TSTI_imm: .byte 1
.global TSTI_off3
TSTI_off3: .byte 0xff

.global x86_CMP_EQ_IMM
x86_CMP_EQ_IMM:
  mov  eax, 0
  movsx eax, al
  cmp  dword ptr [r15], eax
  sete al
  movzx eax, al
  and  ebx, -2
  or   ebx, eax
.size x86_CMP_EQ_IMM, .-x86_CMP_EQ_IMM
.global CMP_EQ_IMM_size
CMP_EQ_IMM_size: .word 22
.global CMP_EQ_IMM_src
CMP_EQ_IMM_src: .byte 0xff
.global CMP_EQ_IMM_dest
CMP_EQ_IMM_dest: .byte 0xff
.global CMP_EQ_IMM_off1
CMP_EQ_IMM_off1: .byte 0xff
.global CMP_EQ_IMM_imm
CMP_EQ_IMM_imm: .byte 1
.global CMP_EQ_IMM_off3
CMP_EQ_IMM_off3: .byte 0xff

.global x86_CMP_PZ
x86_CMP_PZ:
  mov  edx, 0
  cmp  dword ptr [r15+rdx*4], 0
  setns al
  movzx eax, al
  and  ebx, -2
  or   ebx, eax
.size x86_CMP_PZ, .-x86_CMP_PZ
.global CMP_PZ_size
CMP_PZ_size: .word 21
.global CMP_PZ_src
CMP_PZ_src: .byte 0xff
.global CMP_PZ_dest
CMP_PZ_dest: .byte 1
.global CMP_PZ_off1
CMP_PZ_off1: .byte 0xff
.global CMP_PZ_imm
CMP_PZ_imm: .byte 0xff
.global CMP_PZ_off3
CMP_PZ_off3: .byte 0xff

.global x86_CMP_PL
x86_CMP_PL:
  mov  edx, 0
  cmp  dword ptr [r15+rdx*4], 0
  setg al
  movzx eax, al
  and  ebx, -2
  or   ebx, eax
.size x86_CMP_PL, .-x86_CMP_PL
.global CMP_PL_size
CMP_PL_size: .word 21
.global CMP_PL_src
CMP_PL_src: .byte 0xff
.global CMP_PL_dest
CMP_PL_dest: .byte 1
.global CMP_PL_off1
CMP_PL_off1: .byte 0xff
.global CMP_PL_imm
CMP_PL_imm: .byte 0xff
.global CMP_PL_off3
CMP_PL_off3: .byte 0xff

.global x86_SHLL2
x86_SHLL2:
  mov  ecx, 0
  mov  eax, [r15+rcx*4]
  shl eax, 2
  mov  [r15+rcx*4], eax
.size x86_SHLL2, .-x86_SHLL2
.global SHLL2_size
SHLL2_size: .word 16
.global SHLL2_src
SHLL2_src: .byte 0xff
.global SHLL2_dest
SHLL2_dest: .byte 1
.global SHLL2_off1
SHLL2_off1: .byte 0xff
.global SHLL2_imm
SHLL2_imm: .byte 0xff
.global SHLL2_off3
SHLL2_off3: .byte 0xff

.global x86_SHLL8
x86_SHLL8:
  mov  ecx, 0
  mov  eax, [r15+rcx*4]
  shl eax, 8
  mov  [r15+rcx*4], eax
.size x86_SHLL8, .-x86_SHLL8
.global SHLL8_size
SHLL8_size: .word 16
.global SHLL8_src
SHLL8_src: .byte 0xff
.global SHLL8_dest
SHLL8_dest: .byte 1
.global SHLL8_off1
SHLL8_off1: .byte 0xff
.global SHLL8_imm
SHLL8_imm: .byte 0xff
.global SHLL8_off3
SHLL8_off3: .byte 0xff

.global x86_SHLL16
x86_SHLL16:
  mov  ecx, 0
  mov  eax, [r15+rcx*4]
  shl eax, 16
  mov  [r15+rcx*4], eax
.size x86_SHLL16, .-x86_SHLL16
.global SHLL16_size
SHLL16_size: .word 16
.global SHLL16_src
SHLL16_src: .byte 0xff
.global SHLL16_dest
SHLL16_dest: .byte 1
.global SHLL16_off1
SHLL16_off1: .byte 0xff
.global SHLL16_imm
SHLL16_imm: .byte 0xff
.global SHLL16_off3
SHLL16_off3: .byte 0xff

.global x86_SHLR2
x86_SHLR2:
  mov  ecx, 0
  mov  eax, [r15+rcx*4]
  shr eax, 2
  mov  [r15+rcx*4], eax
.size x86_SHLR2, .-x86_SHLR2
.global SHLR2_size
SHLR2_size: .word 16
.global SHLR2_src
SHLR2_src: .byte 0xff
.global SHLR2_dest
SHLR2_dest: .byte 1
.global SHLR2_off1
SHLR2_off1: .byte 0xff
.global SHLR2_imm
SHLR2_imm: .byte 0xff
.global SHLR2_off3
SHLR2_off3: .byte 0xff

.global x86_SHLR8
x86_SHLR8:
  mov  ecx, 0
  mov  eax, [r15+rcx*4]
  shr eax, 8
  mov  [r15+rcx*4], eax
.size x86_SHLR8, .-x86_SHLR8
.global SHLR8_size
SHLR8_size: .word 16
.global SHLR8_src
SHLR8_src: .byte 0xff
.global SHLR8_dest
SHLR8_dest: .byte 1
.global SHLR8_off1
SHLR8_off1: .byte 0xff
.global SHLR8_imm
SHLR8_imm: .byte 0xff
.global SHLR8_off3
SHLR8_off3: .byte 0xff

.global x86_SHLR16
x86_SHLR16:
  mov  ecx, 0
  mov  eax, [r15+rcx*4]
  shr eax, 16
  mov  [r15+rcx*4], eax
.size x86_SHLR16, .-x86_SHLR16
.global SHLR16_size
SHLR16_size: .word 16
.global SHLR16_src
SHLR16_src: .byte 0xff
.global SHLR16_dest
SHLR16_dest: .byte 1
.global SHLR16_off1
SHLR16_off1: .byte 0xff
.global SHLR16_imm
SHLR16_imm: .byte 0xff
.global SHLR16_off3
SHLR16_off3: .byte 0xff

.global x86_ROTCL
x86_ROTCL:
  mov  ecx, 0
  mov  eax, [r15+rcx*4]
  bt   ebx, 0
  rcl  eax, 1
  mov  [r15+rcx*4], eax
  setc dl
  movzx edx, dl
  and  ebx, -2
  or   ebx, edx
.size x86_ROTCL, .-x86_ROTCL
.global ROTCL_size
ROTCL_size: .word 30
.global ROTCL_src
ROTCL_src: .byte 0xff
.global ROTCL_dest
ROTCL_dest: .byte 1
.global ROTCL_off1
ROTCL_off1: .byte 0xff
.global ROTCL_imm
ROTCL_imm: .byte 0xff
.global ROTCL_off3
ROTCL_off3: .byte 0xff

.global x86_ROTCR
x86_ROTCR:
  mov  ecx, 0
  mov  eax, [r15+rcx*4]
  bt   ebx, 0
  rcr  eax, 1
  mov  [r15+rcx*4], eax
  setc dl
  movzx edx, dl
  and  ebx, -2
  or   ebx, edx
.size x86_ROTCR, .-x86_ROTCR
.global ROTCR_size
ROTCR_size: .word 30
.global ROTCR_src
ROTCR_src: .byte 0xff
.global ROTCR_dest
ROTCR_dest: .byte 1
.global ROTCR_off1
ROTCR_off1: .byte 0xff
.global ROTCR_imm
ROTCR_imm: .byte 0xff
.global ROTCR_off3
ROTCR_off3: .byte 0xff

.global x86_MULS
x86_MULS:
  mov  ecx, 0
  mov  edx, 0
  mov  eax, [r15+rcx*4]
  movsx eax, word ptr [r15+rcx*4]
  movsx edx, word ptr [r15+rdx*4]
  imul eax, edx
  mov  [r15+80], eax
.size x86_MULS, .-x86_MULS
.global MULS_size
MULS_size: .word 31
.global MULS_src
MULS_src: .byte 1
.global MULS_dest
MULS_dest: .byte 6
.global MULS_off1
MULS_off1: .byte 0xff
.global MULS_imm
MULS_imm: .byte 0xff
.global MULS_off3
MULS_off3: .byte 0xff

.global x86_MULU
x86_MULU:
  mov  ecx, 0
  mov  edx, 0
  mov  eax, [r15+rcx*4]
  movzx eax, word ptr [r15+rcx*4]
  movzx edx, word ptr [r15+rdx*4]
  imul eax, edx
  mov  [r15+80], eax
.size x86_MULU, .-x86_MULU
.global MULU_size
MULU_size: .word 31
.global MULU_src
MULU_src: .byte 1
.global MULU_dest
MULU_dest: .byte 6
.global MULU_off1
MULU_off1: .byte 0xff
.global MULU_imm
MULU_imm: .byte 0xff
.global MULU_off3
MULU_off3: .byte 0xff

.global x86_MULL
x86_MULL:
  mov  ecx, 0
  mov  edx, 0
  mov  eax, [r15+rcx*4]
  mov  eax, [r15+rcx*4]
  mov  edx, [r15+rdx*4]
  imul eax, edx
  mov  [r15+80], eax
.size x86_MULL, .-x86_MULL
.global MULL_size
MULL_size: .word 29
.global MULL_src
MULL_src: .byte 1
.global MULL_dest
MULL_dest: .byte 6
.global MULL_off1
MULL_off1: .byte 0xff
.global MULL_imm
MULL_imm: .byte 0xff
.global MULL_off3
MULL_off3: .byte 0xff

.global x86_DMULS
x86_DMULS:
  mov  ecx, 0
  mov  edx, 0
  mov  eax, [r15+rcx*4]
  movsxd rax, dword ptr [r15+rcx*4]
  movsxd rdx, dword ptr [r15+rdx*4]
  imul rax, rdx
  mov  [r15+80], eax
  shr  rax, 32
  mov  [r15+76], eax
.size x86_DMULS, .-x86_DMULS
.global DMULS_size
DMULS_size: .word 38
.global DMULS_src
DMULS_src: .byte 1
.global DMULS_dest
DMULS_dest: .byte 6
.global DMULS_off1
DMULS_off1: .byte 0xff
.global DMULS_imm
DMULS_imm: .byte 0xff
.global DMULS_off3
DMULS_off3: .byte 0xff

.global x86_DMULU
x86_DMULU:
  mov  ecx, 0
  mov  edx, 0
  mov  eax, [r15+rcx*4]
  mov  eax, [r15+rcx*4]
  mov  edx, [r15+rdx*4]
  imul rax, rdx
  mov  [r15+80], eax
  shr  rax, 32
  mov  [r15+76], eax
.size x86_DMULU, .-x86_DMULU
.global DMULU_size
DMULU_size: .word 38
.global DMULU_src
DMULU_src: .byte 1
.global DMULU_dest
DMULU_dest: .byte 6
.global DMULU_off1
DMULU_off1: .byte 0xff
.global DMULU_imm
DMULU_imm: .byte 0xff
.global DMULU_off3
DMULU_off3: .byte 0xff

.global x86_MOVBS
x86_MOVBS:
  mov  ecx, 0
  mov  edx, 0
  mov  esi, [r15+rcx*4]
  mov  edi, [r15+rdx*4]
  call qword ptr [r15+128]
.size x86_MOVBS, .-x86_MOVBS
.global MOVBS_size
MOVBS_size: .word 25
.global MOVBS_src
MOVBS_src: .byte 1
.global MOVBS_dest
MOVBS_dest: .byte 6
.global MOVBS_off1
MOVBS_off1: .byte 0xff
.global MOVBS_imm
MOVBS_imm: .byte 0xff
.global MOVBS_off3
MOVBS_off3: .byte 0xff

.global x86_MOVWS
x86_MOVWS:
  mov  ecx, 0
  mov  edx, 0
  mov  esi, [r15+rcx*4]
  mov  edi, [r15+rdx*4]
  call qword ptr [r15+136]
.size x86_MOVWS, .-x86_MOVWS
.global MOVWS_size
MOVWS_size: .word 25
.global MOVWS_src
MOVWS_src: .byte 1
.global MOVWS_dest
MOVWS_dest: .byte 6
.global MOVWS_off1
MOVWS_off1: .byte 0xff
.global MOVWS_imm
MOVWS_imm: .byte 0xff
.global MOVWS_off3
MOVWS_off3: .byte 0xff

.global x86_MOVLS
x86_MOVLS:
  mov  ecx, 0
  mov  edx, 0
  mov  esi, [r15+rcx*4]
  mov  edi, [r15+rdx*4]
  call qword ptr [r15+144]
.size x86_MOVLS, .-x86_MOVLS
.global MOVLS_size
MOVLS_size: .word 25
.global MOVLS_src
MOVLS_src: .byte 1
.global MOVLS_dest
MOVLS_dest: .byte 6
.global MOVLS_off1
MOVLS_off1: .byte 0xff
.global MOVLS_imm
MOVLS_imm: .byte 0xff
.global MOVLS_off3
MOVLS_off3: .byte 0xff

.global x86_MOVBM
x86_MOVBM:
  mov  ecx, 0
  mov  edx, 0
  sub  dword ptr [r15+rdx*4], 1
  mov  edi, [r15+rdx*4]
  mov  esi, [r15+rcx*4]
  call qword ptr [r15+128]
.size x86_MOVBM, .-x86_MOVBM
.global MOVBM_size
MOVBM_size: .word 30
.global MOVBM_src
MOVBM_src: .byte 1
.global MOVBM_dest
MOVBM_dest: .byte 6
.global MOVBM_off1
MOVBM_off1: .byte 0xff
.global MOVBM_imm
MOVBM_imm: .byte 0xff
.global MOVBM_off3
MOVBM_off3: .byte 0xff

.global x86_MOVWM
x86_MOVWM:
  mov  ecx, 0
  mov  edx, 0
  sub  dword ptr [r15+rdx*4], 2
  mov  edi, [r15+rdx*4]
  mov  esi, [r15+rcx*4]
  call qword ptr [r15+136]
.size x86_MOVWM, .-x86_MOVWM
.global MOVWM_size
MOVWM_size: .word 30
.global MOVWM_src
MOVWM_src: .byte 1
.global MOVWM_dest
MOVWM_dest: .byte 6
.global MOVWM_off1
MOVWM_off1: .byte 0xff
.global MOVWM_imm
MOVWM_imm: .byte 0xff
.global MOVWM_off3
MOVWM_off3: .byte 0xff

.global x86_MOVLM
x86_MOVLM:
  mov  ecx, 0
  mov  edx, 0
  sub  dword ptr [r15+rdx*4], 4
  mov  edi, [r15+rdx*4]
  mov  esi, [r15+rcx*4]
  call qword ptr [r15+144]
.size x86_MOVLM, .-x86_MOVLM
.global MOVLM_size
MOVLM_size: .word 30
.global MOVLM_src
MOVLM_src: .byte 1
.global MOVLM_dest
MOVLM_dest: .byte 6
.global MOVLM_off1
MOVLM_off1: .byte 0xff
.global MOVLM_imm
MOVLM_imm: .byte 0xff
.global MOVLM_off3
MOVLM_off3: .byte 0xff

.global x86_MOVBP
x86_MOVBP:
  mov  ecx, 0
  mov  edx, 0
  mov  edi, [r15+rcx*4]
  add  dword ptr [r15+rcx*4], 1
  push rdx
  push rdx
  call qword ptr [r15+104]
  pop  rdx
  pop  rdx
  movsx eax, al
  mov  [r15+rdx*4], eax
.size x86_MOVBP, .-x86_MOVBP
.global MOVBP_size
MOVBP_size: .word 34
.global MOVBP_src
MOVBP_src: .byte 1
.global MOVBP_dest
MOVBP_dest: .byte 6
.global MOVBP_off1
MOVBP_off1: .byte 0xff
.global MOVBP_imm
MOVBP_imm: .byte 0xff
.global MOVBP_off3
MOVBP_off3: .byte 0xff

.global x86_MOVWP
x86_MOVWP:
  mov  ecx, 0
  mov  edx, 0
  mov  edi, [r15+rcx*4]
  add  dword ptr [r15+rcx*4], 2
  push rdx
  push rdx
  call qword ptr [r15+112]
  pop  rdx
  pop  rdx
  movsx eax, ax
  mov  [r15+rdx*4], eax
.size x86_MOVWP, .-x86_MOVWP
.global MOVWP_size
MOVWP_size: .word 34
.global MOVWP_src
MOVWP_src: .byte 1
.global MOVWP_dest
MOVWP_dest: .byte 6
.global MOVWP_off1
MOVWP_off1: .byte 0xff
.global MOVWP_imm
MOVWP_imm: .byte 0xff
.global MOVWP_off3
MOVWP_off3: .byte 0xff

.global x86_MOVLP
x86_MOVLP:
  mov  ecx, 0
  mov  edx, 0
  mov  edi, [r15+rcx*4]
  add  dword ptr [r15+rcx*4], 4
  push rdx
  push rdx
  call qword ptr [r15+120]
  pop  rdx
  pop  rdx
  mov  [r15+rdx*4], eax
.size x86_MOVLP, .-x86_MOVLP
.global MOVLP_size
MOVLP_size: .word 31
.global MOVLP_src
MOVLP_src: .byte 1
.global MOVLP_dest
MOVLP_dest: .byte 6
.global MOVLP_off1
MOVLP_off1: .byte 0xff
.global MOVLP_imm
MOVLP_imm: .byte 0xff
.global MOVLP_off3
MOVLP_off3: .byte 0xff

.global x86_MOVBL
x86_MOVBL:
  mov  ecx, 0
  mov  edx, 0
  mov  edi, [r15+rcx*4]
  push rdx
  push rdx
  call qword ptr [r15+104]
  pop  rdx
  pop  rdx
  movsx eax, al
  mov  [r15+rdx*4], eax
.size x86_MOVBL, .-x86_MOVBL
.global MOVBL_size
MOVBL_size: .word 29
.global MOVBL_src
MOVBL_src: .byte 1
.global MOVBL_dest
MOVBL_dest: .byte 6
.global MOVBL_off1
MOVBL_off1: .byte 0xff
.global MOVBL_imm
MOVBL_imm: .byte 0xff
.global MOVBL_off3
MOVBL_off3: .byte 0xff

.global x86_MOVWL
x86_MOVWL:
  mov  ecx, 0
  mov  edx, 0
  mov  edi, [r15+rcx*4]
  push rdx
  push rdx
  call qword ptr [r15+112]
  pop  rdx
  pop  rdx
  movsx eax, ax
  mov  [r15+rdx*4], eax
.size x86_MOVWL, .-x86_MOVWL
.global MOVWL_size
MOVWL_size: .word 29
.global MOVWL_src
MOVWL_src: .byte 1
.global MOVWL_dest
MOVWL_dest: .byte 6
.global MOVWL_off1
MOVWL_off1: .byte 0xff
.global MOVWL_imm
MOVWL_imm: .byte 0xff
.global MOVWL_off3
MOVWL_off3: .byte 0xff

.global x86_MOVL_MEM_REG
x86_MOVL_MEM_REG:
  mov  ecx, 0
  mov  edx, 0
  mov  edi, [r15+rcx*4]
  push rdx
  push rdx
  call qword ptr [r15+120]
  pop  rdx
  pop  rdx
  mov  [r15+rdx*4], eax
.size x86_MOVL_MEM_REG, .-x86_MOVL_MEM_REG
.global MOVL_MEM_REG_size
MOVL_MEM_REG_size: .word 26
.global MOVL_MEM_REG_src
MOVL_MEM_REG_src: .byte 1
.global MOVL_MEM_REG_dest
MOVL_MEM_REG_dest: .byte 6
.global MOVL_MEM_REG_off1
MOVL_MEM_REG_off1: .byte 0xff
.global MOVL_MEM_REG_imm
MOVL_MEM_REG_imm: .byte 0xff
.global MOVL_MEM_REG_off3
MOVL_MEM_REG_off3: .byte 0xff

.global x86_MOVR
x86_MOVR:
  mov  ecx, 0
  mov  edx, 0
  mov  eax, [r15+rcx*4]
  mov  [r15+rdx*4], eax
.size x86_MOVR, .-x86_MOVR
.global MOVR_size
MOVR_size: .word 18
.global MOVR_src
MOVR_src: .byte 1
.global MOVR_dest
MOVR_dest: .byte 6
.global MOVR_off1
MOVR_off1: .byte 0xff
.global MOVR_imm
MOVR_imm: .byte 0xff
.global MOVR_off3
MOVR_off3: .byte 0xff

.global x86_MOVT
x86_MOVT:
  mov  edx, 0
  mov  eax, ebx
  and  eax, 1
  mov  [r15+rdx*4], eax
.size x86_MOVT, .-x86_MOVT
.global MOVT_size
MOVT_size: .word 14
.global MOVT_src
MOVT_src: .byte 0xff
.global MOVT_dest
MOVT_dest: .byte 1
.global MOVT_off1
MOVT_off1: .byte 0xff
.global MOVT_imm
MOVT_imm: .byte 0xff
.global MOVT_off3
MOVT_off3: .byte 0xff

.global x86_MOVI
x86_MOVI:
  mov  edx, 0
  mov  eax, 0
  movsx eax, al
  mov  [r15+rdx*4], eax
.size x86_MOVI, .-x86_MOVI
.global MOVI_size
MOVI_size: .word 17
.global MOVI_src
MOVI_src: .byte 0xff
.global MOVI_dest
MOVI_dest: .byte 1
.global MOVI_off1
MOVI_off1: .byte 0xff
.global MOVI_imm
MOVI_imm: .byte 6
.global MOVI_off3
MOVI_off3: .byte 0xff

.global x86_MOVBS0
x86_MOVBS0:
  mov  ecx, 0
  mov  edx, 0
  mov  esi, [r15+rcx*4]
  mov  edi, [r15+rdx*4]
  add  edi, [r15]
  call qword ptr [r15+128]
.size x86_MOVBS0, .-x86_MOVBS0
.global MOVBS0_size
MOVBS0_size: .word 28
.global MOVBS0_src
MOVBS0_src: .byte 1
.global MOVBS0_dest
MOVBS0_dest: .byte 6
.global MOVBS0_off1
MOVBS0_off1: .byte 0xff
.global MOVBS0_imm
MOVBS0_imm: .byte 0xff
.global MOVBS0_off3
MOVBS0_off3: .byte 0xff

.global x86_MOVWS0
x86_MOVWS0:
  mov  ecx, 0
  mov  edx, 0
  mov  esi, [r15+rcx*4]
  mov  edi, [r15+rdx*4]
  add  edi, [r15]
  call qword ptr [r15+136]
.size x86_MOVWS0, .-x86_MOVWS0
.global MOVWS0_size
MOVWS0_size: .word 28
.global MOVWS0_src
MOVWS0_src: .byte 1
.global MOVWS0_dest
MOVWS0_dest: .byte 6
.global MOVWS0_off1
MOVWS0_off1: .byte 0xff
.global MOVWS0_imm
MOVWS0_imm: .byte 0xff
.global MOVWS0_off3
MOVWS0_off3: .byte 0xff

.global x86_MOVLS0
x86_MOVLS0:
  mov  ecx, 0
  mov  edx, 0
  mov  esi, [r15+rcx*4]
  mov  edi, [r15+rdx*4]
  add  edi, [r15]
  call qword ptr [r15+144]
.size x86_MOVLS0, .-x86_MOVLS0
.global MOVLS0_size
MOVLS0_size: .word 28
.global MOVLS0_src
MOVLS0_src: .byte 1
.global MOVLS0_dest
MOVLS0_dest: .byte 6
.global MOVLS0_off1
MOVLS0_off1: .byte 0xff
.global MOVLS0_imm
MOVLS0_imm: .byte 0xff
.global MOVLS0_off3
MOVLS0_off3: .byte 0xff

.global x86_MOVBL0
x86_MOVBL0:
  mov  ecx, 0
  mov  edx, 0
  mov  edi, [r15+rcx*4]
  add  edi, [r15]
  push rdx
  push rdx
  call qword ptr [r15+104]
  pop  rdx
  pop  rdx
  movsx eax, al
  mov  [r15+rdx*4], eax
.size x86_MOVBL0, .-x86_MOVBL0
.global MOVBL0_size
MOVBL0_size: .word 32
.global MOVBL0_src
MOVBL0_src: .byte 1
.global MOVBL0_dest
MOVBL0_dest: .byte 6
.global MOVBL0_off1
MOVBL0_off1: .byte 0xff
.global MOVBL0_imm
MOVBL0_imm: .byte 0xff
.global MOVBL0_off3
MOVBL0_off3: .byte 0xff

.global x86_MOVWL0
x86_MOVWL0:
  mov  ecx, 0
  mov  edx, 0
  mov  edi, [r15+rcx*4]
  add  edi, [r15]
  push rdx
  push rdx
  call qword ptr [r15+112]
  pop  rdx
  pop  rdx
  movsx eax, ax
  mov  [r15+rdx*4], eax
.size x86_MOVWL0, .-x86_MOVWL0
.global MOVWL0_size
MOVWL0_size: .word 32
.global MOVWL0_src
MOVWL0_src: .byte 1
.global MOVWL0_dest
MOVWL0_dest: .byte 6
.global MOVWL0_off1
MOVWL0_off1: .byte 0xff
.global MOVWL0_imm
MOVWL0_imm: .byte 0xff
.global MOVWL0_off3
MOVWL0_off3: .byte 0xff

.global x86_MOVLL0
x86_MOVLL0:
  mov  ecx, 0
  mov  edx, 0
  mov  edi, [r15+rcx*4]
  add  edi, [r15]
  push rdx
  push rdx
  call qword ptr [r15+120]
  pop  rdx
  pop  rdx
  mov  [r15+rdx*4], eax
.size x86_MOVLL0, .-x86_MOVLL0
.global MOVLL0_size
MOVLL0_size: .word 29
.global MOVLL0_src
MOVLL0_src: .byte 1
.global MOVLL0_dest
MOVLL0_dest: .byte 6
.global MOVLL0_off1
MOVLL0_off1: .byte 0xff
.global MOVLL0_imm
MOVLL0_imm: .byte 0xff
.global MOVLL0_off3
MOVLL0_off3: .byte 0xff

.global x86_MOVBS4
x86_MOVBS4:
  mov  ecx, 0
  mov  edi, 0
  add  edi, [r15+rcx*4]
  mov  esi, [r15]
  call qword ptr [r15+128]
.size x86_MOVBS4, .-x86_MOVBS4
.global MOVBS4_size
MOVBS4_size: .word 24
.global MOVBS4_src
MOVBS4_src: .byte 1
.global MOVBS4_dest
MOVBS4_dest: .byte 0xff
.global MOVBS4_off1
MOVBS4_off1: .byte 6
.global MOVBS4_imm
MOVBS4_imm: .byte 0xff
.global MOVBS4_off3
MOVBS4_off3: .byte 0xff

.global x86_MOVWS4
x86_MOVWS4:
  mov  ecx, 0
  mov  edi, 0
  add  edi, edi
  add  edi, [r15+rcx*4]
  mov  esi, [r15]
  call qword ptr [r15+136]
.size x86_MOVWS4, .-x86_MOVWS4
.global MOVWS4_size
MOVWS4_size: .word 26
.global MOVWS4_src
MOVWS4_src: .byte 1
.global MOVWS4_dest
MOVWS4_dest: .byte 0xff
.global MOVWS4_off1
MOVWS4_off1: .byte 6
.global MOVWS4_imm
MOVWS4_imm: .byte 0xff
.global MOVWS4_off3
MOVWS4_off3: .byte 0xff

.global x86_MOVBL4
x86_MOVBL4:
  mov  ecx, 0
  mov  edi, 0
  add  edi, [r15+rcx*4]
  call qword ptr [r15+104]
  movsx eax, al
  mov  [r15], eax
.size x86_MOVBL4, .-x86_MOVBL4
.global MOVBL4_size
MOVBL4_size: .word 24
.global MOVBL4_src
MOVBL4_src: .byte 1
.global MOVBL4_dest
MOVBL4_dest: .byte 0xff
.global MOVBL4_off1
MOVBL4_off1: .byte 6
.global MOVBL4_imm
MOVBL4_imm: .byte 0xff
.global MOVBL4_off3
MOVBL4_off3: .byte 0xff

.global x86_MOVWL4
x86_MOVWL4:
  mov  ecx, 0
  mov  edi, 0
  add  edi, edi
  add  edi, [r15+rcx*4]
  call qword ptr [r15+112]
  movsx eax, ax
  mov  [r15], eax
.size x86_MOVWL4, .-x86_MOVWL4
.global MOVWL4_size
MOVWL4_size: .word 26
.global MOVWL4_src
MOVWL4_src: .byte 1
.global MOVWL4_dest
MOVWL4_dest: .byte 0xff
.global MOVWL4_off1
MOVWL4_off1: .byte 6
.global MOVWL4_imm
MOVWL4_imm: .byte 0xff
.global MOVWL4_off3
MOVWL4_off3: .byte 0xff

.global x86_MOVLS4
x86_MOVLS4:
  mov  ecx, 0
  mov  edx, 0
  mov  edi, 0
  shl  edi, 2
  add  edi, [r15+rdx*4]
  mov  esi, [r15+rcx*4]
  call qword ptr [r15+144]
.size x86_MOVLS4, .-x86_MOVLS4
.global MOVLS4_size
MOVLS4_size: .word 33
.global MOVLS4_src
MOVLS4_src: .byte 1
.global MOVLS4_dest
MOVLS4_dest: .byte 6
.global MOVLS4_off1
MOVLS4_off1: .byte 11
.global MOVLS4_imm
MOVLS4_imm: .byte 0xff
.global MOVLS4_off3
MOVLS4_off3: .byte 0xff

.global x86_MOVLL4
x86_MOVLL4:
  mov  ecx, 0
  mov  edx, 0
  mov  edi, 0
  shl  edi, 2
  add  edi, [r15+rcx*4]
  push rdx
  push rdx
  call qword ptr [r15+120]
  pop  rdx
  pop  rdx
  mov  [r15+rdx*4], eax
.size x86_MOVLL4, .-x86_MOVLL4
.global MOVLL4_size
MOVLL4_size: .word 34
.global MOVLL4_src
MOVLL4_src: .byte 1
.global MOVLL4_dest
MOVLL4_dest: .byte 6
.global MOVLL4_off1
MOVLL4_off1: .byte 11
.global MOVLL4_imm
MOVLL4_imm: .byte 0xff
.global MOVLL4_off3
MOVLL4_off3: .byte 0xff

.global x86_MOVBSG
x86_MOVBSG:
  mov  edi, 0
  add  edi, [r15+68]
  mov  esi, [r15]
  call qword ptr [r15+128]
.size x86_MOVBSG, .-x86_MOVBSG
.global MOVBSG_size
MOVBSG_size: .word 19
.global MOVBSG_src
MOVBSG_src: .byte 0xff
.global MOVBSG_dest
MOVBSG_dest: .byte 0xff
.global MOVBSG_off1
MOVBSG_off1: .byte 0xff
.global MOVBSG_imm
MOVBSG_imm: .byte 1
.global MOVBSG_off3
MOVBSG_off3: .byte 0xff

.global x86_MOVWSG
x86_MOVWSG:
  mov  edi, 0
  add  edi, edi
  add  edi, [r15+68]
  mov  esi, [r15]
  call qword ptr [r15+136]
.size x86_MOVWSG, .-x86_MOVWSG
.global MOVWSG_size
MOVWSG_size: .word 21
.global MOVWSG_src
MOVWSG_src: .byte 0xff
.global MOVWSG_dest
MOVWSG_dest: .byte 0xff
.global MOVWSG_off1
MOVWSG_off1: .byte 0xff
.global MOVWSG_imm
MOVWSG_imm: .byte 1
.global MOVWSG_off3
MOVWSG_off3: .byte 0xff

.global x86_MOVLSG
x86_MOVLSG:
  mov  edi, 0
  shl  edi, 2
  add  edi, [r15+68]
  mov  esi, [r15]
  call qword ptr [r15+144]
.size x86_MOVLSG, .-x86_MOVLSG
.global MOVLSG_size
MOVLSG_size: .word 22
.global MOVLSG_src
MOVLSG_src: .byte 0xff
.global MOVLSG_dest
MOVLSG_dest: .byte 0xff
.global MOVLSG_off1
MOVLSG_off1: .byte 0xff
.global MOVLSG_imm
MOVLSG_imm: .byte 1
.global MOVLSG_off3
MOVLSG_off3: .byte 0xff

.global x86_MOVBLG
x86_MOVBLG:
  mov  edi, 0
  add  edi, [r15+68]
  call qword ptr [r15+104]
  movsx eax, al
  mov  [r15], eax
.size x86_MOVBLG, .-x86_MOVBLG
.global MOVBLG_size
MOVBLG_size: .word 19
.global MOVBLG_src
MOVBLG_src: .byte 0xff
.global MOVBLG_dest
MOVBLG_dest: .byte 0xff
.global MOVBLG_off1
MOVBLG_off1: .byte 0xff
.global MOVBLG_imm
MOVBLG_imm: .byte 1
.global MOVBLG_off3
MOVBLG_off3: .byte 0xff

.global x86_MOVWLG
x86_MOVWLG:
  mov  edi, 0
  add  edi, edi
  add  edi, [r15+68]
  call qword ptr [r15+112]
  movsx eax, ax
  mov  [r15], eax
.size x86_MOVWLG, .-x86_MOVWLG
.global MOVWLG_size
MOVWLG_size: .word 21
.global MOVWLG_src
MOVWLG_src: .byte 0xff
.global MOVWLG_dest
MOVWLG_dest: .byte 0xff
.global MOVWLG_off1
MOVWLG_off1: .byte 0xff
.global MOVWLG_imm
MOVWLG_imm: .byte 1
.global MOVWLG_off3
MOVWLG_off3: .byte 0xff

.global x86_MOVLLG
x86_MOVLLG:
  mov  edi, 0
  shl  edi, 2
  add  edi, [r15+68]
  call qword ptr [r15+120]
  mov  [r15], eax
.size x86_MOVLLG, .-x86_MOVLLG
.global MOVLLG_size
MOVLLG_size: .word 19
.global MOVLLG_src
MOVLLG_src: .byte 0xff
.global MOVLLG_dest
MOVLLG_dest: .byte 0xff
.global MOVLLG_off1
MOVLLG_off1: .byte 0xff
.global MOVLLG_imm
MOVLLG_imm: .byte 1
.global MOVLLG_off3
MOVLLG_off3: .byte 0xff

.global x86_MOVA
x86_MOVA:
  mov  eax, 0
  mov  edx, r14d
  add  edx, 4
  and  edx, -4
  lea  edx, [rdx+rax*4]
  mov  [r15], edx
.size x86_MOVA, .-x86_MOVA
.global MOVA_size
MOVA_size: .word 20
.global MOVA_src
MOVA_src: .byte 0xff
.global MOVA_dest
MOVA_dest: .byte 0xff
.global MOVA_off1
MOVA_off1: .byte 0xff
.global MOVA_imm
MOVA_imm: .byte 1
.global MOVA_off3
MOVA_off3: .byte 0xff

.global x86_TAS
x86_TAS:
  mov  edx, 0
  mov  edi, [r15+rdx*4]
  push rdi
  push rdi
  call qword ptr [r15+104]
  pop  rdi
  pop  rdi
  movzx eax, al
  test eax, eax
  sete cl
  movzx ecx, cl
  and  ebx, -2
  or   ebx, ecx
  or   eax, 0x80
  mov  esi, eax
  call qword ptr [r15+128]
.size x86_TAS, .-x86_TAS
.global TAS_size
TAS_size: .word 47
.global TAS_src
TAS_src: .byte 0xff
.global TAS_dest
TAS_dest: .byte 1
.global TAS_off1
TAS_off1: .byte 0xff
.global TAS_imm
TAS_imm: .byte 0xff
.global TAS_off3
TAS_off3: .byte 0xff

.global x86_CLRT
x86_CLRT:
  and  ebx, -2
.size x86_CLRT, .-x86_CLRT
.global CLRT_size
CLRT_size: .word 3
.global CLRT_src
CLRT_src: .byte 0xff
.global CLRT_dest
CLRT_dest: .byte 0xff
.global CLRT_off1
CLRT_off1: .byte 0xff
.global CLRT_imm
CLRT_imm: .byte 0xff
.global CLRT_off3
CLRT_off3: .byte 0xff

.global x86_SETT
x86_SETT:
  or   ebx, 1
.size x86_SETT, .-x86_SETT
.global SETT_size
SETT_size: .word 3
.global SETT_src
SETT_src: .byte 0xff
.global SETT_dest
SETT_dest: .byte 0xff
.global SETT_off1
SETT_off1: .byte 0xff
.global SETT_imm
SETT_imm: .byte 0xff
.global SETT_off3
SETT_off3: .byte 0xff

.global x86_CLRMAC
x86_CLRMAC:
  mov  qword ptr [r15+76], 0
.size x86_CLRMAC, .-x86_CLRMAC
.global CLRMAC_size
CLRMAC_size: .word 8
.global CLRMAC_src
CLRMAC_src: .byte 0xff
.global CLRMAC_dest
CLRMAC_dest: .byte 0xff
.global CLRMAC_off1
CLRMAC_off1: .byte 0xff
.global CLRMAC_imm
CLRMAC_imm: .byte 0xff
.global CLRMAC_off3
CLRMAC_off3: .byte 0xff

.global x86_DIV0U
x86_DIV0U:
  and  ebx, 0xFFFFFCFE
.size x86_DIV0U, .-x86_DIV0U
.global DIV0U_size
DIV0U_size: .word 6
.global DIV0U_src
DIV0U_src: .byte 0xff
.global DIV0U_dest
DIV0U_dest: .byte 0xff
.global DIV0U_off1
DIV0U_off1: .byte 0xff
.global DIV0U_imm
DIV0U_imm: .byte 0xff
.global DIV0U_off3
DIV0U_off3: .byte 0xff

.global x86_SLEEP
x86_SLEEP:
  sub  r14d, 2
.size x86_SLEEP, .-x86_SLEEP
.global SLEEP_size
SLEEP_size: .word 4
.global SLEEP_src
SLEEP_src: .byte 0xff
.global SLEEP_dest
SLEEP_dest: .byte 0xff
.global SLEEP_off1
SLEEP_off1: .byte 0xff
.global SLEEP_imm
SLEEP_imm: .byte 0xff
.global SLEEP_off3
SLEEP_off3: .byte 0xff

.global x86_XTRCT
x86_XTRCT:
  mov  ecx, 0
  mov  edx, 0
  mov  eax, [r15+rcx*4]
  mov  esi, [r15+rdx*4]
  shl  eax, 16
  shr  esi, 16
  or   eax, esi
  mov  [r15+rdx*4], eax
.size x86_XTRCT, .-x86_XTRCT
.global XTRCT_size
XTRCT_size: .word 30
.global XTRCT_src
XTRCT_src: .byte 1
.global XTRCT_dest
XTRCT_dest: .byte 6
.global XTRCT_off1
XTRCT_off1: .byte 0xff
.global XTRCT_imm
XTRCT_imm: .byte 0xff
.global XTRCT_off3
XTRCT_off3: .byte 0xff

.global x86_NEGC
x86_NEGC:
  mov  ecx, 0
  mov  edx, 0
  mov  eax, [r15+rcx*4]
  xor  eax, eax
  bt   ebx, 0
  sbb  eax, [r15+rcx*4]
  mov  [r15+rdx*4], eax
  setc al
  movzx eax, al
  and  ebx, -2
  or   ebx, eax
.size x86_NEGC, .-x86_NEGC
.global NEGC_size
NEGC_size: .word 39
.global NEGC_src
NEGC_src: .byte 1
.global NEGC_dest
NEGC_dest: .byte 6
.global NEGC_off1
NEGC_off1: .byte 0xff
.global NEGC_imm
NEGC_imm: .byte 0xff
.global NEGC_off3
NEGC_off3: .byte 0xff

.global x86_CMPSTR
x86_CMPSTR:
  mov  ecx, 0
  mov  edx, 0
  mov  eax, [r15+rcx*4]
  xor  eax, [r15+rdx*4]
  mov  ecx, eax
  sub  ecx, 0x01010101
  not  eax
  and  ecx, eax
  test ecx, 0x80808080
  setne cl
  movzx ecx, cl
  and  ebx, -2
  or   ebx, ecx
.size x86_CMPSTR, .-x86_CMPSTR
.global CMPSTR_size
CMPSTR_size: .word 47
.global CMPSTR_src
CMPSTR_src: .byte 1
.global CMPSTR_dest
CMPSTR_dest: .byte 6
.global CMPSTR_off1
CMPSTR_off1: .byte 0xff
.global CMPSTR_imm
CMPSTR_imm: .byte 0xff
.global CMPSTR_off3
CMPSTR_off3: .byte 0xff

.global x86_AND_B
x86_AND_B:
  mov  eax, 0
  push rax
  push rax
  mov  edi, [r15]
  add  edi, [r15+68]
  call qword ptr [r15+104]
  movzx esi, al
  pop  rax
  pop  rax
  and  esi, eax
  mov  edi, [r15]
  add  edi, [r15+68]
  call qword ptr [r15+128]
.size x86_AND_B, .-x86_AND_B
.global AND_B_size
AND_B_size: .word 39
.global AND_B_src
AND_B_src: .byte 0xff
.global AND_B_dest
AND_B_dest: .byte 0xff
.global AND_B_off1
AND_B_off1: .byte 0xff
.global AND_B_imm
AND_B_imm: .byte 1
.global AND_B_off3
AND_B_off3: .byte 0xff

.global x86_OR_B
x86_OR_B:
  mov  eax, 0
  push rax
  push rax
  mov  edi, [r15]
  add  edi, [r15+68]
  call qword ptr [r15+104]
  movzx esi, al
  pop  rax
  pop  rax
  or  esi, eax
  mov  edi, [r15]
  add  edi, [r15+68]
  call qword ptr [r15+128]
.size x86_OR_B, .-x86_OR_B
.global OR_B_size
OR_B_size: .word 39
.global OR_B_src
OR_B_src: .byte 0xff
.global OR_B_dest
OR_B_dest: .byte 0xff
.global OR_B_off1
OR_B_off1: .byte 0xff
.global OR_B_imm
OR_B_imm: .byte 1
.global OR_B_off3
OR_B_off3: .byte 0xff

.global x86_XOR_B
x86_XOR_B:
  mov  eax, 0
  push rax
  push rax
  mov  edi, [r15]
  add  edi, [r15+68]
  call qword ptr [r15+104]
  movzx esi, al
  pop  rax
  pop  rax
  xor  esi, eax
  mov  edi, [r15]
  add  edi, [r15+68]
  call qword ptr [r15+128]
.size x86_XOR_B, .-x86_XOR_B
.global XOR_B_size
XOR_B_size: .word 39
.global XOR_B_src
XOR_B_src: .byte 0xff
.global XOR_B_dest
XOR_B_dest: .byte 0xff
.global XOR_B_off1
XOR_B_off1: .byte 0xff
.global XOR_B_imm
XOR_B_imm: .byte 1
.global XOR_B_off3
XOR_B_off3: .byte 0xff

.global x86_TST_B
x86_TST_B:
  mov  eax, 0
  push rax
  push rax
  mov  edi, [r15]
  add  edi, [r15+68]
  call qword ptr [r15+104]
  movzx eax, al
  pop  rdx
  pop  rdx
  test eax, edx
  sete al
  movzx eax, al
  and  ebx, -2
  or   ebx, eax
.size x86_TST_B, .-x86_TST_B
.global TST_B_size
TST_B_size: .word 36
.global TST_B_src
TST_B_src: .byte 0xff
.global TST_B_dest
TST_B_dest: .byte 0xff
.global TST_B_off1
TST_B_off1: .byte 0xff
.global TST_B_imm
TST_B_imm: .byte 1
.global TST_B_off3
TST_B_off3: .byte 0xff

.global x86_BT
x86_BT:
  mov  eax, 0
  movsx eax, al
  lea  eax, [r14+rax*2+4]
  xor  ecx, ecx
  test bl, 1
  cmovz eax, ecx
.size x86_BT, .-x86_BT
.global BT_size
BT_size: .word 21
.global BT_src
BT_src: .byte 0xff
.global BT_dest
BT_dest: .byte 0xff
.global BT_off1
BT_off1: .byte 0xff
.global BT_imm
BT_imm: .byte 1
.global BT_off3
BT_off3: .byte 0xff

.global x86_BF
x86_BF:
  mov  eax, 0
  movsx eax, al
  lea  eax, [r14+rax*2+4]
  xor  ecx, ecx
  test bl, 1
  cmovnz eax, ecx
.size x86_BF, .-x86_BF
.global BF_size
BF_size: .word 21
.global BF_src
BF_src: .byte 0xff
.global BF_dest
BF_dest: .byte 0xff
.global BF_off1
BF_off1: .byte 0xff
.global BF_imm
BF_imm: .byte 1
.global BF_off3
BF_off3: .byte 0xff

.global x86_BF_S
x86_BF_S:
  mov  eax, 0
  movsx eax, al
  lea  eax, [r14+rax*2+4]
  xor  ecx, ecx
  test bl, 1
  cmovnz eax, ecx
.size x86_BF_S, .-x86_BF_S
.global BF_S_size
BF_S_size: .word 21
.global BF_S_src
BF_S_src: .byte 0xff
.global BF_S_dest
BF_S_dest: .byte 0xff
.global BF_S_off1
BF_S_off1: .byte 0xff
.global BF_S_imm
BF_S_imm: .byte 1
.global BF_S_off3
BF_S_off3: .byte 0xff

.global x86_BRA
x86_BRA:
  mov  eax, 0
  shl  eax, 20
  sar  eax, 20
  lea  eax, [r14+rax*2+4]
.size x86_BRA, .-x86_BRA
.global BRA_size
BRA_size: .word 16
.global BRA_src
BRA_src: .byte 0xff
.global BRA_dest
BRA_dest: .byte 0xff
.global BRA_off1
BRA_off1: .byte 0xff
.global BRA_imm
BRA_imm: .byte 0xff
.global BRA_off3
BRA_off3: .byte 1

.global x86_BSR
x86_BSR:
  mov  eax, 0
  shl  eax, 20
  sar  eax, 20
  lea  edx, [r14+4]
  mov  [r15+84], edx
  lea  eax, [rdx+rax*2]
.size x86_BSR, .-x86_BSR
.global BSR_size
BSR_size: .word 22
.global BSR_src
BSR_src: .byte 0xff
.global BSR_dest
BSR_dest: .byte 0xff
.global BSR_off1
BSR_off1: .byte 0xff
.global BSR_imm
BSR_imm: .byte 0xff
.global BSR_off3
BSR_off3: .byte 1

.global x86_JMP
x86_JMP:
  mov  edx, 0
  mov  eax, [r15+rdx*4]
.size x86_JMP, .-x86_JMP
.global JMP_size
JMP_size: .word 9
.global JMP_src
JMP_src: .byte 0xff
.global JMP_dest
JMP_dest: .byte 1
.global JMP_off1
JMP_off1: .byte 0xff
.global JMP_imm
JMP_imm: .byte 0xff
.global JMP_off3
JMP_off3: .byte 0xff

.global x86_JSR
x86_JSR:
  mov  edx, 0
  lea  ecx, [r14+4]
  mov  [r15+84], ecx
  mov  eax, [r15+rdx*4]
.size x86_JSR, .-x86_JSR
.global JSR_size
JSR_size: .word 17
.global JSR_src
JSR_src: .byte 0xff
.global JSR_dest
JSR_dest: .byte 1
.global JSR_off1
JSR_off1: .byte 0xff
.global JSR_imm
JSR_imm: .byte 0xff
.global JSR_off3
JSR_off3: .byte 0xff

.global x86_BRAF
x86_BRAF:
  mov  edx, 0
  mov  eax, [r15+rdx*4]
  lea  eax, [r14+rax+4]
.size x86_BRAF, .-x86_BRAF
.global BRAF_size
BRAF_size: .word 14
.global BRAF_src
BRAF_src: .byte 0xff
.global BRAF_dest
BRAF_dest: .byte 1
.global BRAF_off1
BRAF_off1: .byte 0xff
.global BRAF_imm
BRAF_imm: .byte 0xff
.global BRAF_off3
BRAF_off3: .byte 0xff

.global x86_BSRF
x86_BSRF:
  mov  edx, 0
  lea  ecx, [r14+4]
  mov  [r15+84], ecx
  mov  eax, [r15+rdx*4]
  lea  eax, [r14+rax+4]
.size x86_BSRF, .-x86_BSRF
.global BSRF_size
BSRF_size: .word 22
.global BSRF_src
BSRF_src: .byte 0xff
.global BSRF_dest
BSRF_dest: .byte 1
.global BSRF_off1
BSRF_off1: .byte 0xff
.global BSRF_imm
BSRF_imm: .byte 0xff
.global BSRF_off3
BSRF_off3: .byte 0xff

.global x86_RTE
x86_RTE:
  mov  edi, [r15+60]
  push rax
  push rax
  call qword ptr [r15+120]
  mov  [rsp], rax
  mov  edi, [r15+60]
  add  edi, 4
  mov  [r15+60], edi
  call qword ptr [r15+120]
  mov  ecx, [rsp]
  add  rsp, 16
  mov  edx, [r15+60]
  add  edx, 4
  mov  [r15+60], edx
  and  eax, 0x3F3
  mov  ebx, eax
  mov  eax, ecx
.size x86_RTE, .-x86_RTE
.global RTE_size
RTE_size: .word 56
.global RTE_src
RTE_src: .byte 0xff
.global RTE_dest
RTE_dest: .byte 0xff
.global RTE_off1
RTE_off1: .byte 0xff
.global RTE_imm
RTE_imm: .byte 0xff
.global RTE_off3
RTE_off3: .byte 0xff

.global x86_TRAPA
x86_TRAPA:
  mov  edx, 0
  mov  edi, [r15+60]
  sub  edi, 4
  mov  [r15+60], edi
  mov  esi, ebx
  push rdx
  push rdx
  call qword ptr [r15+144]
  pop  rdx
  pop  rdx
  mov  edi, [r15+60]
  sub  edi, 4
  mov  [r15+60], edi
  lea  esi, [r14+2]
  push rdx
  push rdx
  call qword ptr [r15+144]
  pop  rdx
  pop  rdx
  lea  edi, [rdx*4]
  add  edi, [r15+72]
  call qword ptr [r15+120]
.size x86_TRAPA, .-x86_TRAPA
.global TRAPA_size
TRAPA_size: .word 70
.global TRAPA_src
TRAPA_src: .byte 0xff
.global TRAPA_dest
TRAPA_dest: .byte 0xff
.global TRAPA_off1
TRAPA_off1: .byte 0xff
.global TRAPA_imm
TRAPA_imm: .byte 1
.global TRAPA_off3
TRAPA_off3: .byte 0xff

.global x86_MOVWI
x86_MOVWI:
  mov  ecx, 0
  mov  edx, 0
  lea  edi, [r14+rcx*2+4]
  push rdx
  push rdx
  call qword ptr [r15+112]
  pop  rdx
  pop  rdx
  movsx eax, ax
  mov  [r15+rdx*4], eax
.size x86_MOVWI, .-x86_MOVWI
.global MOVWI_size
MOVWI_size: .word 30
.global MOVWI_src
MOVWI_src: .byte 0xff
.global MOVWI_dest
MOVWI_dest: .byte 6
.global MOVWI_off1
MOVWI_off1: .byte 0xff
.global MOVWI_imm
MOVWI_imm: .byte 1
.global MOVWI_off3
MOVWI_off3: .byte 0xff

.global x86_MOVLI
x86_MOVLI:
  mov  ecx, 0
  mov  edx, 0
  lea  edi, [r14+4]
  and  edi, -4
  lea  edi, [rdi+rcx*4]
  push rdx
  push rdx
  call qword ptr [r15+120]
  pop  rdx
  pop  rdx
  mov  [r15+rdx*4], eax
.size x86_MOVLI, .-x86_MOVLI
.global MOVLI_size
MOVLI_size: .word 32
.global MOVLI_src
MOVLI_src: .byte 0xff
.global MOVLI_dest
MOVLI_dest: .byte 6
.global MOVLI_off1
MOVLI_off1: .byte 0xff
.global MOVLI_imm
MOVLI_imm: .byte 1
.global MOVLI_off3
MOVLI_off3: .byte 0xff

.global x86_STC_SR
x86_STC_SR:
  mov  edx, 0
  mov  eax, ebx
  mov  [r15+rdx*4], eax
.size x86_STC_SR, .-x86_STC_SR
.global STC_SR_size
STC_SR_size: .word 11
.global STC_SR_src
STC_SR_src: .byte 0xff
.global STC_SR_dest
STC_SR_dest: .byte 1
.global STC_SR_off1
STC_SR_off1: .byte 0xff
.global STC_SR_imm
STC_SR_imm: .byte 0xff
.global STC_SR_off3
STC_SR_off3: .byte 0xff

.global x86_STC_GBR
x86_STC_GBR:
  mov  edx, 0
  mov  eax, [r15+68]
  mov  [r15+rdx*4], eax
.size x86_STC_GBR, .-x86_STC_GBR
.global STC_GBR_size
STC_GBR_size: .word 13
.global STC_GBR_src
STC_GBR_src: .byte 0xff
.global STC_GBR_dest
STC_GBR_dest: .byte 1
.global STC_GBR_off1
STC_GBR_off1: .byte 0xff
.global STC_GBR_imm
STC_GBR_imm: .byte 0xff
.global STC_GBR_off3
STC_GBR_off3: .byte 0xff

.global x86_STC_VBR
x86_STC_VBR:
  mov  edx, 0
  mov  eax, [r15+72]
  mov  [r15+rdx*4], eax
.size x86_STC_VBR, .-x86_STC_VBR
.global STC_VBR_size
STC_VBR_size: .word 13
.global STC_VBR_src
STC_VBR_src: .byte 0xff
.global STC_VBR_dest
STC_VBR_dest: .byte 1
.global STC_VBR_off1
STC_VBR_off1: .byte 0xff
.global STC_VBR_imm
STC_VBR_imm: .byte 0xff
.global STC_VBR_off3
STC_VBR_off3: .byte 0xff

.global x86_STS_MACH
x86_STS_MACH:
  mov  edx, 0
  mov  eax, [r15+76]
  mov  [r15+rdx*4], eax
.size x86_STS_MACH, .-x86_STS_MACH
.global STS_MACH_size
STS_MACH_size: .word 13
.global STS_MACH_src
STS_MACH_src: .byte 0xff
.global STS_MACH_dest
STS_MACH_dest: .byte 1
.global STS_MACH_off1
STS_MACH_off1: .byte 0xff
.global STS_MACH_imm
STS_MACH_imm: .byte 0xff
.global STS_MACH_off3
STS_MACH_off3: .byte 0xff

.global x86_STS_MACL
x86_STS_MACL:
  mov  edx, 0
  mov  eax, [r15+80]
  mov  [r15+rdx*4], eax
.size x86_STS_MACL, .-x86_STS_MACL
.global STS_MACL_size
STS_MACL_size: .word 13
.global STS_MACL_src
STS_MACL_src: .byte 0xff
.global STS_MACL_dest
STS_MACL_dest: .byte 1
.global STS_MACL_off1
STS_MACL_off1: .byte 0xff
.global STS_MACL_imm
STS_MACL_imm: .byte 0xff
.global STS_MACL_off3
STS_MACL_off3: .byte 0xff

.global x86_STS_PR
x86_STS_PR:
  mov  edx, 0
  mov  eax, [r15+84]
  mov  [r15+rdx*4], eax
.size x86_STS_PR, .-x86_STS_PR
.global STS_PR_size
STS_PR_size: .word 13
.global STS_PR_src
STS_PR_src: .byte 0xff
.global STS_PR_dest
STS_PR_dest: .byte 1
.global STS_PR_off1
STS_PR_off1: .byte 0xff
.global STS_PR_imm
STS_PR_imm: .byte 0xff
.global STS_PR_off3
STS_PR_off3: .byte 0xff

.global x86_STC_SR_MEM
x86_STC_SR_MEM:
  mov  edx, 0
  sub  dword ptr [r15+rdx*4], 4
  mov  edi, [r15+rdx*4]
  mov  esi, ebx
  call qword ptr [r15+144]
.size x86_STC_SR_MEM, .-x86_STC_SR_MEM
.global STC_SR_MEM_size
STC_SR_MEM_size: .word 23
.global STC_SR_MEM_src
STC_SR_MEM_src: .byte 0xff
.global STC_SR_MEM_dest
STC_SR_MEM_dest: .byte 1
.global STC_SR_MEM_off1
STC_SR_MEM_off1: .byte 0xff
.global STC_SR_MEM_imm
STC_SR_MEM_imm: .byte 0xff
.global STC_SR_MEM_off3
STC_SR_MEM_off3: .byte 0xff

.global x86_STC_GBR_MEM
x86_STC_GBR_MEM:
  mov  edx, 0
  sub  dword ptr [r15+rdx*4], 4
  mov  edi, [r15+rdx*4]
  mov  esi, [r15+68]
  call qword ptr [r15+144]
.size x86_STC_GBR_MEM, .-x86_STC_GBR_MEM
.global STC_GBR_MEM_size
STC_GBR_MEM_size: .word 25
.global STC_GBR_MEM_src
STC_GBR_MEM_src: .byte 0xff
.global STC_GBR_MEM_dest
STC_GBR_MEM_dest: .byte 1
.global STC_GBR_MEM_off1
STC_GBR_MEM_off1: .byte 0xff
.global STC_GBR_MEM_imm
STC_GBR_MEM_imm: .byte 0xff
.global STC_GBR_MEM_off3
STC_GBR_MEM_off3: .byte 0xff

.global x86_STC_VBR_MEM
x86_STC_VBR_MEM:
  mov  edx, 0
  sub  dword ptr [r15+rdx*4], 4
  mov  edi, [r15+rdx*4]
  mov  esi, [r15+72]
  call qword ptr [r15+144]
.size x86_STC_VBR_MEM, .-x86_STC_VBR_MEM
.global STC_VBR_MEM_size
STC_VBR_MEM_size: .word 25
.global STC_VBR_MEM_src
STC_VBR_MEM_src: .byte 0xff
.global STC_VBR_MEM_dest
STC_VBR_MEM_dest: .byte 1
.global STC_VBR_MEM_off1
STC_VBR_MEM_off1: .byte 0xff
.global STC_VBR_MEM_imm
STC_VBR_MEM_imm: .byte 0xff
.global STC_VBR_MEM_off3
STC_VBR_MEM_off3: .byte 0xff

.global x86_STS_MACH_DEC
x86_STS_MACH_DEC:
  mov  edx, 0
  sub  dword ptr [r15+rdx*4], 4
  mov  edi, [r15+rdx*4]
  mov  esi, [r15+76]
  call qword ptr [r15+144]
.size x86_STS_MACH_DEC, .-x86_STS_MACH_DEC
.global STS_MACH_DEC_size
STS_MACH_DEC_size: .word 25
.global STS_MACH_DEC_src
STS_MACH_DEC_src: .byte 0xff
.global STS_MACH_DEC_dest
STS_MACH_DEC_dest: .byte 1
.global STS_MACH_DEC_off1
STS_MACH_DEC_off1: .byte 0xff
.global STS_MACH_DEC_imm
STS_MACH_DEC_imm: .byte 0xff
.global STS_MACH_DEC_off3
STS_MACH_DEC_off3: .byte 0xff

.global x86_STS_MACL_DEC
x86_STS_MACL_DEC:
  mov  edx, 0
  sub  dword ptr [r15+rdx*4], 4
  mov  edi, [r15+rdx*4]
  mov  esi, [r15+80]
  call qword ptr [r15+144]
.size x86_STS_MACL_DEC, .-x86_STS_MACL_DEC
.global STS_MACL_DEC_size
STS_MACL_DEC_size: .word 25
.global STS_MACL_DEC_src
STS_MACL_DEC_src: .byte 0xff
.global STS_MACL_DEC_dest
STS_MACL_DEC_dest: .byte 1
.global STS_MACL_DEC_off1
STS_MACL_DEC_off1: .byte 0xff
.global STS_MACL_DEC_imm
STS_MACL_DEC_imm: .byte 0xff
.global STS_MACL_DEC_off3
STS_MACL_DEC_off3: .byte 0xff

.global x86_STSMPR
x86_STSMPR:
  mov  edx, 0
  sub  dword ptr [r15+rdx*4], 4
  mov  edi, [r15+rdx*4]
  mov  esi, [r15+84]
  call qword ptr [r15+144]
.size x86_STSMPR, .-x86_STSMPR
.global STSMPR_size
STSMPR_size: .word 25
.global STSMPR_src
STSMPR_src: .byte 0xff
.global STSMPR_dest
STSMPR_dest: .byte 1
.global STSMPR_off1
STSMPR_off1: .byte 0xff
.global STSMPR_imm
STSMPR_imm: .byte 0xff
.global STSMPR_off3
STSMPR_off3: .byte 0xff

.global x86_LDCGBR
x86_LDCGBR:
  mov  edx, 0
  mov  eax, [r15+rdx*4]
  mov  [r15+68], eax
.size x86_LDCGBR, .-x86_LDCGBR
.global LDCGBR_size
LDCGBR_size: .word 13
.global LDCGBR_src
LDCGBR_src: .byte 0xff
.global LDCGBR_dest
LDCGBR_dest: .byte 1
.global LDCGBR_off1
LDCGBR_off1: .byte 0xff
.global LDCGBR_imm
LDCGBR_imm: .byte 0xff
.global LDCGBR_off3
LDCGBR_off3: .byte 0xff

.global x86_LDC_VBR
x86_LDC_VBR:
  mov  edx, 0
  mov  eax, [r15+rdx*4]
  mov  [r15+72], eax
.size x86_LDC_VBR, .-x86_LDC_VBR
.global LDC_VBR_size
LDC_VBR_size: .word 13
.global LDC_VBR_src
LDC_VBR_src: .byte 0xff
.global LDC_VBR_dest
LDC_VBR_dest: .byte 1
.global LDC_VBR_off1
LDC_VBR_off1: .byte 0xff
.global LDC_VBR_imm
LDC_VBR_imm: .byte 0xff
.global LDC_VBR_off3
LDC_VBR_off3: .byte 0xff

.global x86_LDS_MACH
x86_LDS_MACH:
  mov  edx, 0
  mov  eax, [r15+rdx*4]
  mov  [r15+76], eax
.size x86_LDS_MACH, .-x86_LDS_MACH
.global LDS_MACH_size
LDS_MACH_size: .word 13
.global LDS_MACH_src
LDS_MACH_src: .byte 0xff
.global LDS_MACH_dest
LDS_MACH_dest: .byte 1
.global LDS_MACH_off1
LDS_MACH_off1: .byte 0xff
.global LDS_MACH_imm
LDS_MACH_imm: .byte 0xff
.global LDS_MACH_off3
LDS_MACH_off3: .byte 0xff

.global x86_LDS_MACL
x86_LDS_MACL:
  mov  edx, 0
  mov  eax, [r15+rdx*4]
  mov  [r15+80], eax
.size x86_LDS_MACL, .-x86_LDS_MACL
.global LDS_MACL_size
LDS_MACL_size: .word 13
.global LDS_MACL_src
LDS_MACL_src: .byte 0xff
.global LDS_MACL_dest
LDS_MACL_dest: .byte 1
.global LDS_MACL_off1
LDS_MACL_off1: .byte 0xff
.global LDS_MACL_imm
LDS_MACL_imm: .byte 0xff
.global LDS_MACL_off3
LDS_MACL_off3: .byte 0xff

.global x86_LDS_PR
x86_LDS_PR:
  mov  edx, 0
  mov  eax, [r15+rdx*4]
  mov  [r15+84], eax
.size x86_LDS_PR, .-x86_LDS_PR
.global LDS_PR_size
LDS_PR_size: .word 13
.global LDS_PR_src
LDS_PR_src: .byte 0xff
.global LDS_PR_dest
LDS_PR_dest: .byte 1
.global LDS_PR_off1
LDS_PR_off1: .byte 0xff
.global LDS_PR_imm
LDS_PR_imm: .byte 0xff
.global LDS_PR_off3
LDS_PR_off3: .byte 0xff

.global x86_LDC_SR
x86_LDC_SR:
  mov  edx, 0
  mov  eax, [r15+rdx*4]
  and  eax, 0x3F3
  mov  ebx, eax
.size x86_LDC_SR, .-x86_LDC_SR
.global LDC_SR_size
LDC_SR_size: .word 16
.global LDC_SR_src
LDC_SR_src: .byte 0xff
.global LDC_SR_dest
LDC_SR_dest: .byte 1
.global LDC_SR_off1
LDC_SR_off1: .byte 0xff
.global LDC_SR_imm
LDC_SR_imm: .byte 0xff
.global LDC_SR_off3
LDC_SR_off3: .byte 0xff

.global x86_LDC_GBR_INC
x86_LDC_GBR_INC:
  mov  edx, 0
  mov  edi, [r15+rdx*4]
  add  dword ptr [r15+rdx*4], 4
  call qword ptr [r15+120]
  mov  [r15+68], eax
.size x86_LDC_GBR_INC, .-x86_LDC_GBR_INC
.global LDC_GBR_INC_size
LDC_GBR_INC_size: .word 22
.global LDC_GBR_INC_src
LDC_GBR_INC_src: .byte 0xff
.global LDC_GBR_INC_dest
LDC_GBR_INC_dest: .byte 1
.global LDC_GBR_INC_off1
LDC_GBR_INC_off1: .byte 0xff
.global LDC_GBR_INC_imm
LDC_GBR_INC_imm: .byte 0xff
.global LDC_GBR_INC_off3
LDC_GBR_INC_off3: .byte 0xff

.global x86_LDC_VBR_INC
x86_LDC_VBR_INC:
  mov  edx, 0
  mov  edi, [r15+rdx*4]
  add  dword ptr [r15+rdx*4], 4
  call qword ptr [r15+120]
  mov  [r15+72], eax
.size x86_LDC_VBR_INC, .-x86_LDC_VBR_INC
.global LDC_VBR_INC_size
LDC_VBR_INC_size: .word 22
.global LDC_VBR_INC_src
LDC_VBR_INC_src: .byte 0xff
.global LDC_VBR_INC_dest
LDC_VBR_INC_dest: .byte 1
.global LDC_VBR_INC_off1
LDC_VBR_INC_off1: .byte 0xff
.global LDC_VBR_INC_imm
LDC_VBR_INC_imm: .byte 0xff
.global LDC_VBR_INC_off3
LDC_VBR_INC_off3: .byte 0xff

.global x86_LDS_MACH_INC
x86_LDS_MACH_INC:
  mov  edx, 0
  mov  edi, [r15+rdx*4]
  add  dword ptr [r15+rdx*4], 4
  call qword ptr [r15+120]
  mov  [r15+76], eax
.size x86_LDS_MACH_INC, .-x86_LDS_MACH_INC
.global LDS_MACH_INC_size
LDS_MACH_INC_size: .word 22
.global LDS_MACH_INC_src
LDS_MACH_INC_src: .byte 0xff
.global LDS_MACH_INC_dest
LDS_MACH_INC_dest: .byte 1
.global LDS_MACH_INC_off1
LDS_MACH_INC_off1: .byte 0xff
.global LDS_MACH_INC_imm
LDS_MACH_INC_imm: .byte 0xff
.global LDS_MACH_INC_off3
LDS_MACH_INC_off3: .byte 0xff

.global x86_LDS_MACL_INC
x86_LDS_MACL_INC:
  mov  edx, 0
  mov  edi, [r15+rdx*4]
  add  dword ptr [r15+rdx*4], 4
  call qword ptr [r15+120]
  mov  [r15+80], eax
.size x86_LDS_MACL_INC, .-x86_LDS_MACL_INC
.global LDS_MACL_INC_size
LDS_MACL_INC_size: .word 22
.global LDS_MACL_INC_src
LDS_MACL_INC_src: .byte 0xff
.global LDS_MACL_INC_dest
LDS_MACL_INC_dest: .byte 1
.global LDS_MACL_INC_off1
LDS_MACL_INC_off1: .byte 0xff
.global LDS_MACL_INC_imm
LDS_MACL_INC_imm: .byte 0xff
.global LDS_MACL_INC_off3
LDS_MACL_INC_off3: .byte 0xff

.global x86_LDS_PR_INC
x86_LDS_PR_INC:
  mov  edx, 0
  mov  edi, [r15+rdx*4]
  add  dword ptr [r15+rdx*4], 4
  call qword ptr [r15+120]
  mov  [r15+84], eax
.size x86_LDS_PR_INC, .-x86_LDS_PR_INC
.global LDS_PR_INC_size
LDS_PR_INC_size: .word 22
.global LDS_PR_INC_src
LDS_PR_INC_src: .byte 0xff
.global LDS_PR_INC_dest
LDS_PR_INC_dest: .byte 1
.global LDS_PR_INC_off1
LDS_PR_INC_off1: .byte 0xff
.global LDS_PR_INC_imm
LDS_PR_INC_imm: .byte 0xff
.global LDS_PR_INC_off3
LDS_PR_INC_off3: .byte 0xff

.global x86_LDC_SR_INC
x86_LDC_SR_INC:
  mov  edx, 0
  mov  edi, [r15+rdx*4]
  add  dword ptr [r15+rdx*4], 4
  call qword ptr [r15+120]
  and  eax, 0x3F3
  mov  ebx, eax
.size x86_LDC_SR_INC, .-x86_LDC_SR_INC
.global LDC_SR_INC_size
LDC_SR_INC_size: .word 25
.global LDC_SR_INC_src
LDC_SR_INC_src: .byte 0xff
.global LDC_SR_INC_dest
LDC_SR_INC_dest: .byte 1
.global LDC_SR_INC_off1
LDC_SR_INC_off1: .byte 0xff
.global LDC_SR_INC_imm
LDC_SR_INC_imm: .byte 0xff
.global LDC_SR_INC_off3
LDC_SR_INC_off3: .byte 0xff

.global x86_DIV0S
x86_DIV0S:
  mov  ecx, 0
  mov  edx, 0
  mov  eax, [r15+rdx*4]
  shr  eax, 31
  mov  esi, [r15+rcx*4]
  shr  esi, 31
  mov  edi, eax
  xor  edi, esi
  and  ebx, 0xFFFFFCFE
  or   ebx, edi
  shl  eax, 8
  or   ebx, eax
  shl  esi, 9
  or   ebx, esi
.size x86_DIV0S, .-x86_DIV0S
.global DIV0S_size
DIV0S_size: .word 46
.global DIV0S_src
DIV0S_src: .byte 1
.global DIV0S_dest
DIV0S_dest: .byte 6
.global DIV0S_off1
DIV0S_off1: .byte 0xff
.global DIV0S_imm
DIV0S_imm: .byte 0xff
.global DIV0S_off3
DIV0S_off3: .byte 0xff

.global x86_DIV1
x86_DIV1:
  mov  ecx, 0
  mov  edx, 0
  mov  r8d, [r15+rcx*4]
  mov  r9d, [r15+rdx*4]
  mov  r10d, r9d
  shr  r10d, 31
  mov  eax, ebx
  and  eax, 1
  add  r9d, r9d
  or   r9d, eax
  mov  [r15+rdx*4], r9d
  mov  r11d, ebx
  shr  r11d, 9
  and  r11d, 1
  mov  eax, ebx
  shr  eax, 8
  and  eax, 1
  test eax, eax
  jz   .Ldiv1_oldq0
  test r11d, r11d
  jz   .Ldiv1_m0_add
  mov  eax, r9d
  sub  r9d, r8d
  mov  [r15+rdx*4], r9d
  cmp  eax, r9d
  setb cl
  jmp  .Ldiv1_sel_eq
.Ldiv1_oldq0:
  test r11d, r11d
  jz   .Ldiv1_m0_sub
  mov  eax, r9d
  add  r9d, r8d
  mov  [r15+rdx*4], r9d
  cmp  eax, r9d
  seta cl
.Ldiv1_sel_eq:
  movzx ecx, cl
  mov  eax, ecx
  xor  eax, 1
  test r10d, r10d
  cmovz ecx, eax
  jmp  .Ldiv1_fin
.Ldiv1_m0_add:
  mov  eax, r9d
  add  r9d, r8d
  mov  [r15+rdx*4], r9d
  cmp  eax, r9d
  seta cl
  jmp  .Ldiv1_sel_ne
.Ldiv1_m0_sub:
  mov  eax, r9d
  sub  r9d, r8d
  mov  [r15+rdx*4], r9d
  cmp  eax, r9d
  setb cl
.Ldiv1_sel_ne:
  movzx ecx, cl
  mov  eax, ecx
  xor  eax, 1
  test r10d, r10d
  cmovnz ecx, eax
.Ldiv1_fin:
  cmp  ecx, r11d
  sete al
  movzx eax, al
  and  ebx, 0xFFFFFEFE
  or   ebx, eax
  mov  eax, ecx
  shl  eax, 8
  or   ebx, eax
.size x86_DIV1, .-x86_DIV1
.global DIV1_size
DIV1_size: .word 195
.global DIV1_src
DIV1_src: .byte 1
.global DIV1_dest
DIV1_dest: .byte 6
.global DIV1_off1
DIV1_off1: .byte 0xff
.global DIV1_imm
DIV1_imm: .byte 0xff
.global DIV1_off3
DIV1_off3: .byte 0xff

.global x86_MAC_L
x86_MAC_L:
  mov  ecx, 0
  mov  edx, 0
  mov  edi, [r15+rdx*4]
  add  dword ptr [r15+rdx*4], 4
  mov  esi, [r15+rcx*4]
  add  dword ptr [r15+rcx*4], 4
  push rsi
  push rsi
  call qword ptr [r15+120]
  pop  rsi
  pop  rsi
  mov  edi, esi
  push rax
  push rax
  call qword ptr [r15+120]
  pop  r8
  pop  r8
  movsxd rax, eax
  movsxd r8, r8d
  imul rax, r8
  mov  ecx, [r15+80]
  mov  edx, [r15+76]
  shl  rdx, 32
  or   rcx, rdx
  add  rcx, rax
  test ebx, 2
  jz   .Lmacl_store
  mov  rdx, 0x00007FFFFFFFFFFF
  cmp  rcx, rdx
  jbe  .Lmacl_store
  mov  rdx, 0xFFFF800000000000
  cmp  rcx, rdx
  jae  .Lmacl_store
  mov  rdx, 0xFFFF800000000000
  test rax, rax
  js   .Lmacl_set
  mov  rdx, 0x00007FFFFFFFFFFF
.Lmacl_set:
  mov  rcx, rdx
.Lmacl_store:
  mov  [r15+80], ecx
  shr  rcx, 32
  mov  [r15+76], ecx
.size x86_MAC_L, .-x86_MAC_L
.global MAC_L_size
MAC_L_size: .word 154
.global MAC_L_src
MAC_L_src: .byte 1
.global MAC_L_dest
MAC_L_dest: .byte 6
.global MAC_L_off1
MAC_L_off1: .byte 0xff
.global MAC_L_imm
MAC_L_imm: .byte 0xff
.global MAC_L_off3
MAC_L_off3: .byte 0xff

.global x86_MAC_W
x86_MAC_W:
  mov  ecx, 0
  mov  edx, 0
  mov  edi, [r15+rcx*4]
  add  dword ptr [r15+rcx*4], 2
  mov  esi, [r15+rdx*4]
  add  dword ptr [r15+rdx*4], 2
  push rsi
  push rsi
  call qword ptr [r15+112]
  pop  rsi
  pop  rsi
  movsx eax, ax
  push rax
  push rax
  mov  edi, esi
  call qword ptr [r15+112]
  pop  r8
  pop  r8
  movsx eax, ax
  movsx r8d, r8w
  imul eax, r8d
  movsxd rax, eax
  mov  ecx, [r15+80]
  movsxd rcx, ecx
  add  rcx, rax
  test ebx, 2
  jz   .Lmacw_nosat
  mov  rdx, 0x000000007FFFFFFF
  cmp  rcx, rdx
  jbe  .Lmacw_store
  mov  rdx, 0xFFFFFFFF80000000
  cmp  rcx, rdx
  jae  .Lmacw_store
  or   dword ptr [r15+76], 1
  mov  rdx, 0x000000007FFFFFFF
  test rax, rax
  jns  .Lmacw_set
  mov  rdx, 0x80000000
.Lmacw_set:
  mov  rcx, rdx
  jmp  .Lmacw_store
.Lmacw_nosat:
  mov  eax, ecx
  shr  rcx, 32
  mov  [r15+76], ecx
  mov  ecx, eax
.Lmacw_store:
  mov  [r15+80], ecx
.size x86_MAC_W, .-x86_MAC_W
.global MAC_W_size
MAC_W_size: .word 155
.global MAC_W_src
MAC_W_src: .byte 1
.global MAC_W_dest
MAC_W_dest: .byte 6
.global MAC_W_off1
MAC_W_off1: .byte 0xff
.global MAC_W_imm
MAC_W_imm: .byte 0xff
.global MAC_W_off3
MAC_W_off3: .byte 0xff

.global x86_NOP
x86_NOP:
  nop
.size x86_NOP, .-x86_NOP
.global NOP_size
NOP_size: .word 1
.global NOP_src
NOP_src: .byte 0xff
.global NOP_dest
NOP_dest: .byte 0xff
.global NOP_off1
NOP_off1: .byte 0xff
.global NOP_imm
NOP_imm: .byte 0xff
.global NOP_off3
NOP_off3: .byte 0xff

.global x86_RTS
x86_RTS:
  mov  eax, [r15+84]
.size x86_RTS, .-x86_RTS
.global RTS_size
RTS_size: .word 4
.global RTS_src
RTS_src: .byte 0xff
.global RTS_dest
RTS_dest: .byte 0xff
.global RTS_off1
RTS_off1: .byte 0xff
.global RTS_imm
RTS_imm: .byte 0xff
.global RTS_off3
RTS_off3: .byte 0xff

/* ---- not implemented yet: trap so a wrong block is visible ---- */

/* Fast memory access paths.  These are installed as the tagSH2 getmem and
   setmem function pointers and are only ever called from generated code, so
   r15 is the tagSH2
   of the running core (hiwram_base at +168, lowram_base at +176, presence bits
   at +184) and the SysV register convention of the block applies (edi = addr,
   esi = value, result in eax).
   Only the cases that the C functions would serve without any extra work are
   done here; everything else tail-jumps to the C function, so values, cycle
   accounting and invalidation stay identical:
     read  0x060 (no cycles: bit 29 is clear), and for the "4"/"5" variants
           0x002 as well; "5" also the on-chip registers 0xFFFFFE00..0xFFFFFFFF
     write 0x060 when no translated block watches the word, i.e. the presence
           bit (addr & 0xFFFFF) >> 1 is clear; otherwise C has to invalidate
   T2ReadLong swaps halfwords, T2ReadWord/T2WriteWord are plain 16-bit
   accesses and T2Read/WriteByte go through mem[addr ^ 1]. */

/* reads: 0x060 in the plain variants, 0x060 or 0x002 in the "4"/"5" ones,
   and the on-chip range in the "5" ones.  rax ends up as the work RAM base,
   ecx as the offset inside it. */
.global memGetLongFast
memGetLongFast:
  mov  eax, edi
  shr  eax, 20
  and  eax, 0xFFF
  cmp  eax, 0x060
  jne  .Lfg_l_c
  mov  rax, [r15+168]
  mov  ecx, edi
  and  ecx, 0xFFFFF
  mov  eax, [rax+rcx]
  ror  eax, 16
  ret
.Lfg_l_c:
  jmp  memGetLong

.global memGetWordFast
memGetWordFast:
  mov  eax, edi
  shr  eax, 20
  and  eax, 0xFFF
  cmp  eax, 0x060
  jne  .Lfg_w_c
  mov  rax, [r15+168]
  mov  ecx, edi
  and  ecx, 0xFFFFF
  movzx eax, word ptr [rax+rcx]
  ret
.Lfg_w_c:
  jmp  memGetWord

.global memGetByteFast
memGetByteFast:
  mov  eax, edi
  shr  eax, 20
  and  eax, 0xFFF
  cmp  eax, 0x060
  jne  .Lfg_b_c
  mov  rax, [r15+168]
  mov  ecx, edi
  and  ecx, 0xFFFFF
  xor  ecx, 1
  movzx eax, byte ptr [rax+rcx]
  ret
.Lfg_b_c:
  jmp  memGetByte

.global memGetLongFast4
memGetLongFast4:
  mov  eax, edi
  shr  eax, 20
  and  eax, 0xFFF
  cmp  eax, 0x060
  jne  1f
  mov  rax, [r15+168]
  jmp  2f
1:
  cmp  eax, 0x002
  jne  .Lfg_l4_c
  mov  rax, [r15+176]
2:
  mov  ecx, edi
  and  ecx, 0xFFFFF
  mov  eax, [rax+rcx]
  ror  eax, 16
  ret
.Lfg_l4_c:
  jmp  memGetLong

.global memGetWordFast4
memGetWordFast4:
  mov  eax, edi
  shr  eax, 20
  and  eax, 0xFFF
  cmp  eax, 0x060
  jne  1f
  mov  rax, [r15+168]
  jmp  2f
1:
  cmp  eax, 0x002
  jne  .Lfg_w4_c
  mov  rax, [r15+176]
2:
  mov  ecx, edi
  and  ecx, 0xFFFFF
  movzx eax, word ptr [rax+rcx]
  ret
.Lfg_w4_c:
  jmp  memGetWord

.global memGetByteFast4
memGetByteFast4:
  mov  eax, edi
  shr  eax, 20
  and  eax, 0xFFF
  cmp  eax, 0x060
  jne  1f
  mov  rax, [r15+168]
  jmp  2f
1:
  cmp  eax, 0x002
  jne  .Lfg_b4_c
  mov  rax, [r15+176]
2:
  mov  ecx, edi
  and  ecx, 0xFFFFF
  xor  ecx, 1
  movzx eax, byte ptr [rax+rcx]
  ret
.Lfg_b4_c:
  jmp  memGetByte

.global memGetLongFast5
memGetLongFast5:
  mov  eax, edi
  shr  eax, 20
  and  eax, 0xFFF
  cmp  eax, 0x060
  jne  1f
  mov  rax, [r15+168]
  jmp  2f
1:
  cmp  eax, 0x002
  jne  .Lfg_l5_chip
  mov  rax, [r15+176]
2:
  mov  ecx, edi
  and  ecx, 0xFFFFF
  mov  eax, [rax+rcx]
  ror  eax, 16
  ret
.Lfg_l5_chip:
  cmp  edi, 0xFFFFFE00
  jb   .Lfg_l5_c
  and  edi, 0x1FF
  jmp  OnchipReadLong
.Lfg_l5_c:
  jmp  memGetLong

.global memGetWordFast5
memGetWordFast5:
  mov  eax, edi
  shr  eax, 20
  and  eax, 0xFFF
  cmp  eax, 0x060
  jne  1f
  mov  rax, [r15+168]
  jmp  2f
1:
  cmp  eax, 0x002
  jne  .Lfg_w5_chip
  mov  rax, [r15+176]
2:
  mov  ecx, edi
  and  ecx, 0xFFFFF
  movzx eax, word ptr [rax+rcx]
  ret
.Lfg_w5_chip:
  cmp  edi, 0xFFFFFE00
  jb   .Lfg_w5_c
  and  edi, 0x1FF
  jmp  OnchipReadWord
.Lfg_w5_c:
  jmp  memGetWord

.global memGetByteFast5
memGetByteFast5:
  mov  eax, edi
  shr  eax, 20
  and  eax, 0xFFF
  cmp  eax, 0x060
  jne  1f
  mov  rax, [r15+168]
  jmp  2f
1:
  cmp  eax, 0x002
  jne  .Lfg_b5_chip
  mov  rax, [r15+176]
2:
  mov  ecx, edi
  and  ecx, 0xFFFFF
  xor  ecx, 1
  movzx eax, byte ptr [rax+rcx]
  ret
.Lfg_b5_chip:
  cmp  edi, 0xFFFFFE00
  jb   .Lfg_b5_c
  and  edi, 0x1FF
  jmp  OnchipReadByte
.Lfg_b5_c:
  jmp  memGetByte

/* writes: only HWRAM, and only when nothing watches the word */
.global memSetLongFast
memSetLongFast:
  mov  eax, edi
  shr  eax, 20
  and  eax, 0xFFF
  cmp  eax, 0x060
  jne  .Lfs_l_c
  mov  rax, [r15+184]
  mov  ecx, edi
  and  ecx, 0xFFFFF
  shr  ecx, 1
  mov  edx, ecx
  shr  edx, 5
  mov  edx, [rax+rdx*4]
  bt   edx, ecx   /* CF = bit (idx & 31) of the bitmap word */
  jc   .Lfs_l_c
  inc  ecx
  mov  edx, ecx
  shr  edx, 5
  mov  edx, [rax+rdx*4]
  bt   edx, ecx   /* CF = bit (idx & 31) of the bitmap word */
  jc   .Lfs_l_c
  mov  rax, [r15+168]
  mov  ecx, edi
  and  ecx, 0xFFFFF
  mov  edx, esi
  ror  edx, 16
  mov  [rax+rcx], edx
  ret
.Lfs_l_c:
  jmp  memSetLong

.global memSetWordFast
memSetWordFast:
  mov  eax, edi
  shr  eax, 20
  and  eax, 0xFFF
  cmp  eax, 0x060
  jne  .Lfs_w_c
  mov  rax, [r15+184]
  mov  ecx, edi
  and  ecx, 0xFFFFF
  shr  ecx, 1
  mov  edx, ecx
  shr  edx, 5
  mov  edx, [rax+rdx*4]
  bt   edx, ecx   /* CF = bit (idx & 31) of the bitmap word */
  jc   .Lfs_w_c
  mov  rax, [r15+168]
  mov  ecx, edi
  and  ecx, 0xFFFFF
  mov  [rax+rcx], si
  ret
.Lfs_w_c:
  jmp  memSetWord

.global memSetByteFast
memSetByteFast:
  mov  eax, edi
  shr  eax, 20
  and  eax, 0xFFF
  cmp  eax, 0x060
  jne  .Lfs_b_c
  mov  rax, [r15+184]
  mov  ecx, edi
  and  ecx, 0xFFFFF
  shr  ecx, 1
  mov  edx, ecx
  shr  edx, 5
  mov  edx, [rax+rdx*4]
  bt   edx, ecx   /* CF = bit (idx & 31) of the bitmap word */
  jc   .Lfs_b_c
  mov  rax, [r15+168]
  mov  ecx, edi
  and  ecx, 0xFFFFF
  xor  ecx, 1
  mov  [rax+rcx], sil
  ret
.Lfs_b_c:
  jmp  memSetByte

/* Chaining decision, reached from the _k exits with the state in the
   registers.  Enter the next block directly only when C would compile and
   call it as well: HWRAM PC, already translated, not an idle-loop block, no
   accepted interrupt pending, and the time slice not over yet.  Otherwise
   store the state and return, i.e. exactly what the old epilogue did.
   Entering behind the prologue keeps the current frame and the live
   r15/r14/r13/r12/ebx, which is what the prologue would reload anyway. */
.global chain_stub
chain_stub:
  mov  rax, [r15+208]            /* cur_block                       */
  test dword ptr [rax+2060], 1   /* BLOCK_LOOP -> let C skip idly   */
  jnz  .Lchain_c
  mov  eax, [r15+96]             /* ICOUNT                          */
  mov  ecx, ebx
  and  ecx, 0xF0
  cmp  ecx, eax                  /* (SR&0xF0) < ICOUNT -> IRQ waits */
  jb   .Lchain_c
  mov  eax, [r15+192]            /* memcycle                        */
  add  eax, r13d
  cmp  eax, r12d                 /* COUNT + memcycle >= exitcount   */
  jae  .Lchain_c
  mov  edx, r14d
  and  edx, 0x0FF00000           /* only HWRAM blocks are chained   */
  cmp  edx, 0x06000000
  jne  .Lchain_c
  mov  rsi, [r15+200]            /* &LookupTable[0]                 */
  mov  edx, r14d
  and  edx, 0x000FFFFF
  shr  edx, 1
  mov  rdx, [rsi+rdx*8]          /* next block or NULL              */
  test rdx, rdx
  jz   .Lchain_c
  mov  r13d, eax                 /* COUNT += memcycle               */
  mov  dword ptr [r15+192], 0
  mov  [r15+208], rdx            /* cur_block = next                */
  mov  [r15+88], r14d
  mov  [r15+92], r13d
  mov  [r15+64], ebx
  add  rdx, 31                   /* PROLOGSIZE                      */
  jmp  rdx
.Lchain_c:
  mov  [r15+88], r14d
  mov  [r15+92], r13d
  mov  [r15+64], ebx
  pop  r15
  pop  r14
  pop  r13
  pop  r12
  pop  rbx
  ret
.size chain_stub, .-chain_stub
