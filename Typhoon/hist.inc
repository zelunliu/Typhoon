; @file  hist.inc
; @brief histogram functions
;
; @author Zelun Liu (Texas A&M University)
; @author Arif Arman (Texas A&M University)
; @author Dmitri Loguinov (Texas A&M University)
;
; Copyright (C) 2025 - 2026 Zelun Liu, Arif Arman, and Dmitri Loguinov.
; All rights reserved.
;
; The 3-clause BSD License is applied to this software, see
; license.txt

KEY_SIZE_BYTES	= (1 SHL KEY_SIZE_POWER)
KEY_SIZE_BITS = (8 * KEY_SIZE_BYTES)

; ============================================================================================================
; asm_hist_SIMD_uint: handles L3, jumps between slices
; ============================================================================================================
s7 SEGMENT align(64) EXECUTE
align 64
IF KEY_SIZE_BYTES EQ 8
	BULK_NOP 16
ELSE
	IF SSE_HIST_EXTRACT EQ 1
	ELSE
		BULK_NOP 12
	ENDIF
ENDIF

STAGGER	= 8			; amount of bytes by which to offset each histogram from its default starting point
OFFLOAD_SIMD32 MACRO reg, hist, x0, x1, x2, x3
	; arguments: xmm register, followed by 4 histograms to use for distributing its keys
	PEXTRB eax, reg, 3
	inc QWORD PTR [hist + x0*(256*8 + STAGGER) + rax*8]

	PEXTRB eax, reg, 7
	inc QWORD PTR [hist + x1*(256*8 + STAGGER) + rax*8]

	PEXTRB eax, reg, 11
	inc QWORD PTR [hist + x2*(256*8 + STAGGER) + rax*8]

	PEXTRB eax, reg, 15
	inc QWORD PTR [hist + x3*(256*8 + STAGGER) + rax*8]
endm

OFFLOAD_SIMD64 MACRO reg, hist, x0, x1
	PEXTRB eax, reg, 7											; LSD directly grabs byte 7
	inc QWORD PTR [hist + x0*(256*8 + STAGGER) + rax*8]			
	PEXTRB eax, reg, 15											; LSD directly grabs byte 15
	inc QWORD PTR [hist + x1*(256*8 + STAGGER) + rax*8]			
endm

OFFLOAD_SCALAR8 MACRO input, hist, x0, x1, x2, x3, x4, x5, x6, x7		; rax, rbx, rsi, rdi, r9-r12
	movzx REG(rax), BYTE PTR [input + (KEY_SIZE_BYTES-1)]
	movzx REG(rbx), BYTE PTR [input + (KEY_SIZE_BYTES-1) + KEY_SIZE_BYTES]
	movzx REG(rdi), BYTE PTR [input + (KEY_SIZE_BYTES-1) + 2*KEY_SIZE_BYTES]
	movzx REG(rsi), BYTE PTR [input + (KEY_SIZE_BYTES-1) + 3*KEY_SIZE_BYTES]
	
	movzx REG(r9), BYTE PTR [input + (KEY_SIZE_BYTES-1) + 4*KEY_SIZE_BYTES]
	movzx REG(r10), BYTE PTR [input + (KEY_SIZE_BYTES-1) + 5*KEY_SIZE_BYTES]
	movzx REG(r11), BYTE PTR [input + (KEY_SIZE_BYTES-1) + 6*KEY_SIZE_BYTES]
	movzx REG(r12), BYTE PTR [input + (KEY_SIZE_BYTES-1) + 7*KEY_SIZE_BYTES]

	add input, 8*KEY_SIZE_BYTES

	inc QWORD PTR [hist + x0*(256*8 + STAGGER) + rax*8]
	inc QWORD PTR [hist + x1*(256*8 + STAGGER) + rbx*8]
	inc QWORD PTR [hist + x2*(256*8 + STAGGER) + rdi*8]
	inc QWORD PTR [hist + x3*(256*8 + STAGGER) + rsi*8]

	inc QWORD PTR [hist + x4*(256*8 + STAGGER) + r9*8]
	inc QWORD PTR [hist + x5*(256*8 + STAGGER) + r10*8]
	inc QWORD PTR [hist + x6*(256*8 + STAGGER) + r11*8]
	inc QWORD PTR [hist + x7*(256*8 + STAGGER) + r12*8]
endm

HIST_FUNC_NAME(asm_L3_uint) proc
	; rcx = input slices
	; rdx = hist 

	input equ <r13>
	slice_array equ <rcx>
	cnt equ <rdx>
	prefetch_distance equ <r14>
	end_slice equ <r8>

	push rbx
	push rdi
	push rsi
	push r12
	push r13
	push r14
	push r15

	; 2 bottom bits of pointer: 00 = full & not last; 01 = partial & last; 02 = full & last
	NEXT_INPUT_SLICE:
		mov input, QWORD PTR [slice_array]

		test input, 1			; last slice?
		jnz FINAL_SLICE 

		mov prefetch_distance, [slice_array + 8]												; 4KB forward

		sub prefetch_distance, input
		add slice_array, 8

		AGAIN:
			IF PREFETCH_ALL_LEVEL EQ 1
				prefetch [input + prefetch_distance]
			ELSE
				prefetchT2 [input + prefetch_distance]
			ENDIF

			IF SSE_HIST_EXTRACT EQ 1
				MOVDQU xmm0, XMMWORD PTR [input]
				MOVDQU xmm1, XMMWORD PTR [input + 16]
				MOVDQU xmm2, XMMWORD PTR [input + 32]
				MOVDQU xmm3, XMMWORD PTR [input + 48]
				add input, 64			; NOTE: cannot unroll by more than 64 bytes due to stagger

				IF KEY_SIZE_BYTES EQ 4
					OFFLOAD_SIMD32 xmm0, cnt, 0, 1, 2, 3	
					OFFLOAD_SIMD32 xmm1, cnt, 4, 5, 6, 7	
					OFFLOAD_SIMD32 xmm2, cnt, 0, 2, 4, 6	
					OFFLOAD_SIMD32 xmm3, cnt, 1, 3, 5, 7	
				ELSE
					OFFLOAD_SIMD64 xmm0, cnt, 0, 1	
					OFFLOAD_SIMD64 xmm1, cnt, 2, 3	
					OFFLOAD_SIMD64 xmm2, cnt, 4, 5	
					OFFLOAD_SIMD64 xmm3, cnt, 6, 7	
				ENDIF
			ELSE
				OFFLOAD_SCALAR8 input, cnt, 0, 1, 2, 3, 4, 5, 6, 7			
			ENDIF

			test input, SLICE_SIZE-1
			jnz	AGAIN

		jmp NEXT_INPUT_SLICE

	IF KEY_SIZE_BYTES EQ 4
		BULK_NOP 3
	ENDIF

FINAL_SLICE:
	lea end_slice, [input + SLICE_SIZE]
	and end_slice, NOT (SLICE_SIZE - 1)		; clear stagger from slice
	
	test input, 2							
	cmovz end_slice, [slice_array + 8]		; partial

	and input, NOT 3						; the final slice is offset by 1 or 3 from the correct start

	TAIL_LOOP:
		movzx rax, BYTE PTR [input + KEY_SIZE_BYTES-1]
		inc QWORD PTR [cnt + rax*8]
		add input, KEY_SIZE_BYTES
		cmp input, end_slice
		jb TAIL_LOOP

	pop r15
	pop r14
	pop r13
	pop r12
	pop rsi
	pop rdi
	pop rbx

	ret
HIST_FUNC_NAME(asm_L3_uint) endp
s7 ENDS

; ============================================================================================================
; asm_hist_simple_SIMD_uint: handles L3 (single-threaded), scans over contiguous space
; ============================================================================================================
s7s SEGMENT align(64) EXECUTE
align 64

BULK_NOP 2
IF KEY_SIZE_BYTES EQ 8
	BULK_NOP 10
ELSE
	IF SSE_HIST_EXTRACT EQ 1
		BULK_NOP 2
	ELSE
		BULK_NOP 10
	ENDIF
ENDIF

HIST_FUNC_NAME(asm_L3_single_uint) proc
	; this runs when nThreads = 1; contiguous slices of keys, except for some limited empty space in between
	; rcx = input slices
	; rdx = cnt hist 
	; r8 = slicesInJob

	slice_array equ <rcx>
	cnt equ <rdx>
	input equ <r13>
	prefetch_distance equ <r14>
	end_slice equ <r15>
	slicesInJob equ <r8>

	; NOTE: scalar histogram destroys rax, rbx, rsi, rdi, r9-r12

	push rbx
	push rbp
	push rdi
	push rsi
	push r12
	push r13
	push r14
	push r15

	NEXT_INPUT_SLICE:
		mov input, QWORD PTR [slice_array]
		mov end_slice, QWORD PTR [slice_array + 8]
		add slice_array, 16

		mov rbp, end_slice
		IF SSE_HIST_EXTRACT EQ 1
			and end_slice, NOT 63						; SIMD always takes 64 bytes
		ELSE
			and end_slice, NOT (8*KEY_SIZE_BYTES-1)		; scalar takes 8 keys
		ENDIF

		cmp input, end_slice				; slice smaller than one iteration of the loop?
		je TAIL_LOOP

		AGAIN:
			IF PREFETCH_ALL_LEVEL EQ 1
				prefetch [input + 4096]
			ELSE
				prefetchT2 [input + 16384]
			ENDIF

			IF SSE_HIST_EXTRACT EQ 1
				VMOVDQU xmm0, XMMWORD PTR [input]
				VMOVDQU xmm1, XMMWORD PTR [input + 16]
				VMOVDQU xmm2, XMMWORD PTR [input + 32]
				VMOVDQU xmm3, XMMWORD PTR [input + 48]

				add input, 64			; NOTE: cannot unroll by more than 64 bytes due to stagger

				IF KEY_SIZE_BYTES EQ 4
					OFFLOAD_SIMD32 xmm0, cnt, 0, 1, 2, 3	
					OFFLOAD_SIMD32 xmm1, cnt, 4, 5, 6, 7	
					OFFLOAD_SIMD32 xmm2, cnt, 0, 2, 4, 6	
					OFFLOAD_SIMD32 xmm3, cnt, 1, 3, 5, 7	
				ELSE
					OFFLOAD_SIMD64 xmm0, cnt, 0, 1	
					OFFLOAD_SIMD64 xmm1, cnt, 2, 3	
					OFFLOAD_SIMD64 xmm2, cnt, 4, 5	
					OFFLOAD_SIMD64 xmm3, cnt, 6, 7	
				ENDIF
			ELSE
				OFFLOAD_SCALAR8 input, cnt, 0, 1, 2, 3, 4, 5, 6, 7			
			ENDIF

			cmp input, end_slice
			jnz	AGAIN

		cmp input, rbp
		jne TAIL_LOOP

	GO_BACK:
		dec slicesInJob
		jnz NEXT_INPUT_SLICE

	pop r15
	pop r14
	pop r13
	pop r12
	pop rsi
	pop rdi
	pop rbp
	pop rbx

	ret

TAIL_LOOP:
		movzx rax, BYTE PTR [input + KEY_SIZE_BYTES-1]
		inc QWORD PTR [cnt + rax*8]
		add input, KEY_SIZE_BYTES
		cmp input, rbp
		jb TAIL_LOOP

	jmp GO_BACK

HIST_FUNC_NAME(asm_L3_single_uint) endp
s7s ENDS