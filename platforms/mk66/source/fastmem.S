@ Word-wise memcpy and memset for the TRIM=1 image (linked in only by platforms/mk66/makefile with TRIM=1).
@
@ Why: newlib-nano (-specs=nano.specs) replaces memcpy / memset with plain byte loops (ldrb + strb, about 6 cycles per
@ byte), the stock link uses newlib's word-wise ones. The audio interrupt calls them every block: AudioStream::receiveWritable
@ (256 B copy of a shared block), MonoToStereo, the bypass copy of AudioFilterStateVariable, AudioOutputStream (3 x 256 B),
@ applyGain / applyGainAndCopy (256 / 512 B clear of a muted channel), the envelope clear. That is the likeliest cause
@ of the TRIM image burning ~15 points more audio CPU than stock with the same project (synth-open/README.md, Idle audio load).
@
@ Same contract as libc: memcpy does not handle overlap, both return the destination. Aligned fast path 4 words per
@ iteration (ldm / stm, about 0.9 cycle per byte), words for a tail of 4..15 bytes, bytes otherwise. A misaligned source or
@ destination takes the byte loop (no unaligned word access: it would trap with UNALIGN_TRP and is slower on the bus anyway).
@ Defining the symbols here makes the linker skip newlib-nano's memcpy.o / memset.o (it only pulls archive members for
@ undefined symbols). memmove, memcmp and strlen stay newlib-nano's, none of them is on the audio path.
#ifdef FW_TRIM
	.cpu	cortex-m4		@ the platform's .S rule passes no -mcpu (same as 3rdparty/Audio/memcpy_audio.S)
	.syntax unified
	.thumb
	.text

	.global	memcpy
	.type	memcpy, %function
	.thumb_func
	.p2align 2
memcpy:
	orr	r3, r0, r1
	push	{r0, r4, r5}		@ r0: return value (push does not touch the flags)
	lsls	r3, r3, #30		@ Z set when both pointers are 4-byte aligned
	bne	.Lcp_bytes
	subs	r2, r2, #16
	blo	.Lcp_w16_end
.Lcp_w16:
	ldmia	r1!, {r3, r4, r5, r12}
	stmia	r0!, {r3, r4, r5, r12}
	subs	r2, r2, #16
	bhs	.Lcp_w16
.Lcp_w16_end:
	adds	r2, r2, #16
.Lcp_w4:
	subs	r2, r2, #4
	blo	.Lcp_w4_end
	ldr	r3, [r1], #4
	str	r3, [r0], #4
	b	.Lcp_w4
.Lcp_w4_end:
	adds	r2, r2, #4
.Lcp_bytes:
	cbz	r2, .Lcp_done
.Lcp_b:
	ldrb	r3, [r1], #1
	strb	r3, [r0], #1
	subs	r2, r2, #1
	bne	.Lcp_b
.Lcp_done:
	pop	{r0, r4, r5}
	bx	lr
	.size	memcpy, . - memcpy

	.global	memset
	.type	memset, %function
	.thumb_func
	.p2align 2
memset:
	push	{r4, r5}
	mov	r12, r0			@ r0 stays the return value
	uxtb	r1, r1
	orr	r1, r1, r1, lsl #8
	orr	r1, r1, r1, lsl #16	@ the fill byte in all four lanes
	tst	r12, #3
	beq	.Lms_aligned
.Lms_head:				@ bytes until the pointer is 4-byte aligned (or n is used up)
	cbz	r2, .Lms_done
	strb	r1, [r12], #1
	subs	r2, r2, #1
	tst	r12, #3
	bne	.Lms_head
.Lms_aligned:
	mov	r3, r1
	mov	r4, r1
	mov	r5, r1
	subs	r2, r2, #16
	blo	.Lms_w16_end
.Lms_w16:
	stmia	r12!, {r1, r3, r4, r5}
	subs	r2, r2, #16
	bhs	.Lms_w16
.Lms_w16_end:
	adds	r2, r2, #16
.Lms_w4:
	subs	r2, r2, #4
	blo	.Lms_w4_end
	str	r1, [r12], #4
	b	.Lms_w4
.Lms_w4_end:
	adds	r2, r2, #4
	cbz	r2, .Lms_done
.Lms_b:
	strb	r1, [r12], #1
	subs	r2, r2, #1
	bne	.Lms_b
.Lms_done:
	pop	{r4, r5}
	bx	lr
	.size	memset, . - memset
#endif
