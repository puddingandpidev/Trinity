/*
 * read_gadgets_gas.s
 *
 * GAS (AT&T) port of ReadGadgets.asm for MinGW-w64 builds.
 * The MASM original (external/api_hashing/ReadGadgets.asm) is used for MSVC builds.
 *
 * { START OF FILE }
 */

    .section .data
    .globl  g_qGadgetQwordAddress
g_qGadgetQwordAddress:
    .quad   0
    .globl  g_qGadgetDwordAddress
g_qGadgetDwordAddress:
    .quad   0
    .globl  g_qGadgetWordAddress
g_qGadgetWordAddress:
    .quad   0

    .section .text

    .globl  SetGadget
SetGadget:
        movq    %rcx, g_qGadgetQwordAddress(%rip)
        movq    %rdx, g_qGadgetDwordAddress(%rip)
        movq    %r8,  g_qGadgetWordAddress(%rip)
        ret
    .globl  ReadQwordViaGadget
ReadQwordViaGadget:
        movq    g_qGadgetQwordAddress(%rip), %rdx
        testq   %rdx, %rdx
        je      .Lno_qword_gadget
        movq    %rcx, %rax
        jmp     *%rdx
.Lno_qword_gadget:
        movq    (%rcx), %rax
        ret
    .globl  ReadDwordViaGadget
ReadDwordViaGadget:
        movq    g_qGadgetDwordAddress(%rip), %rdx
        testq   %rdx, %rdx
        je      .Lno_dword_gadget
        movq    %rcx, %rax
        jmp     *%rdx
.Lno_dword_gadget:
        jmp     ReadQwordViaGadget
    .globl  ReadWordViaGadget
ReadWordViaGadget:
        movq    g_qGadgetWordAddress(%rip), %rdx
        testq   %rdx, %rdx
        je      .Lno_word_gadget
        movq    %rcx, %rax
        jmp     *%rdx
.Lno_word_gadget:
        jmp     ReadDwordViaGadget
/*
 * { END OF FILE }
 */
