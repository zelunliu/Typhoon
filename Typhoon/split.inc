; @file  split.inc
; @brief splitter functions
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
WC_LINE = (1 SHL WC_LINE_BITS)		

; ============================================================================================================
; WCv2_L0: handle L0 (sequential input + prefetch)
; ============================================================================================================
WCv2L0 SEGMENT align(64) EXECUTE
align 64
	BULK_NOP 1

AsmParams3 STRUCT
	asm_input			QWORD 		?
	asm_end_input		QWORD		?
	asm_pNext			QWORD		?
	asm_digit			QWORD 		?
	asm_stmp			QWORD 		?
	asm_free_stack		QWORD 		?
	asm_sd				QWORD		?
AsmParams3 ENDS

SPLIT_FUNC_NAME(asm_L0_uint) proc
	; input (rcx, rdx, r8, r9) + (rax, r10, r11, xmm4, xmm5) are volatile
	; rcx = AsmParams3* ptr

	; WCv2_aligned_static uses 9 registers + r10 is tmp
	input				equ <rcx>			; once per key
	pNext				equ <r8>			; once per wcLine
	digit				equ <r9>			; once per key
	stmp				equ <r14>			; once per key
	p0					equ <r12>			; once per key
	p1					equ <r15>			; once per key
	idx0				equ <r11>			; once per key
	idx0b				equ <r11b>
	idx1				equ <rdi>			; once per key
	idx1b				equ <dil>
	end_input			equ <r13>			; once per slice; can be reused on the last slice

	; WCv2_slices: needs an additional 2 registers (free_stack, sd)
	free_stack			equ <rbx>			; once per slice
	sd					equ <rbp>			; once per slice

	; unused = rax, rdx, rsi; tmp usage = r10

	push rbx
	push rbp
	push rsi
	push rdi
	push r12
	push r13
	push r14
	push r15

	push rcx			; save pointer to struct so that we can update its values

	mov sd, QWORD PTR [rcx].AsmParams3.asm_sd					; once per output slice
	mov pNext, QWORD PTR [rcx].AsmParams3.asm_pNext				; once per wcLine
	mov digit, QWORD PTR [rcx].AsmParams3.asm_digit				; once per key
	mov stmp, QWORD PTR [rcx].AsmParams3.asm_stmp				; once per key
	mov free_stack, QWORD PTR [rcx].AsmParams3.asm_free_stack	; once per output slice
	mov end_input, QWORD PTR [rcx].AsmParams3.asm_end_input
	; this overwrites rcx, so it must be the last parameter to be initalized
	mov input, QWORD PTR [rcx].AsmParams3.asm_input				; once per input slice

	; NOTE: job partitioning ensures each thread gets a full slice, so no sharing of slices
	; NOTE: input ptr in the first slice is always aligned to 2 keys, so we never cross slice boundaries on odd keys

	push end_input
	and end_input, NOT (SLICE_SIZE-1)
	; if the entire input is 1 slice, jump to the end
	cmp input, end_input								
	je TAIL

	AGAIN:
		prefetchT2 QWORD PTR [input + 4096]
	
		movzx idx0, BYTE PTR [input + digit]						
		movzx idx1, BYTE PTR [input + digit + KEY_SIZE_BYTES]		
		mov p0, QWORD PTR [stmp + idx0*8]			; p0 = stmp[buck0]
		mov p1, QWORD PTR [stmp + idx1*8]			; p1 = stmp[buck1] 

		mov REG(r10), [input]						; r10 = key0		
		add input, 2*KEY_SIZE_BYTES
		mov [p0], REG(r10)							; save key0

		add p0, KEY_SIZE_BYTES
		test p0, WC_LINE * KEY_SIZE_BYTES - 1
		jz OFFLOAD1									; r10 is available

		; ----------------------------
	PROCESS_2ND_KEY:
		cmp idx1, idx0
		cmove p1, p0

		mov REG(r10), [input - KEY_SIZE_BYTES]		; r10 = key1	
		mov [p1], REG(r10)							; save key1

		add p1, KEY_SIZE_BYTES
		mov QWORD PTR [stmp + idx0*8], p0				; stmp[idx0] = p0
		test p1, WC_LINE * KEY_SIZE_BYTES - 1
		jz OFFLOAD2										; r10, r11 are available

		mov QWORD PTR [stmp + idx1*8], p1				; stmp[buck1] = p1
	
	CONTINUE:
		test input, SLICE_SIZE-1
		jnz	AGAIN

		; jump back to the start of slice and save it into free stack
		lea r10, [input - SLICE_SIZE]
		mov [free_stack], r10
		add free_stack, 8

		cmp input, end_input
		jb AGAIN

TAIL:
	pop end_input

	cmp input, end_input		; finish on a full slice?
	je SKIP

	; the final slice is processed using WCv1
	AGAIN_FINAL:
		movzx idx0, BYTE PTR [input + digit]						
		mov p0, QWORD PTR [stmp + idx0*8]			; stmp[buck0] = p0

		mov REG(r10), [input]						; r10 = key0		
		add input, KEY_SIZE_BYTES
		mov [p0], REG(r10)							; save key0

		add p0, KEY_SIZE_BYTES
		test p0, WC_LINE * KEY_SIZE_BYTES - 1
		jz OFFLOAD3
	
		mov QWORD PTR [stmp + idx0*8], p0				; stmp[buck1] = p1

	CONTINUE_FINAL:
		cmp input, end_input
		jb	AGAIN_FINAL

	dec input								; round down to start of slice
	and input, NOT (SLICE_SIZE-1)
	mov [free_stack], input
	add free_stack, 8

SKIP:
	pop rcx
	mov QWORD PTR [rcx].AsmParams3.asm_sd, sd
	mov rax, free_stack						; return the free-stack pointer

	pop r15
	pop r14
	pop r13
	pop r12
	pop rdi
	pop rsi
	pop rbp
	pop rbx

	;vzeroupper								; on Haswell and older CPUs, the histogram gets slow if mixing SSE and AVX instructions

	ret

	; ---------------------------------------------------------------------------------------------------
	BULK_NOP 7

OFFLOAD1:
	mov r10, QWORD PTR [pNext + idx0*8]			; r10 = pNext[idx0] start pointer
	sub p0, WC_LINE * KEY_SIZE_BYTES

	test r10, SLICE_SIZE - 1					; pNext is NULL initially or aligned to slice boundary when crossing slices
	jz GET_OUTPUT_SLICE1

MOVE_WC1:
	i = 0
	while i LT WC_LINE * KEY_SIZE_BYTES
		VMOVDQU ymm0, YMMWORD PTR [p0 + i] 
		VMOVNTDQ YMMWORD PTR [r10 + i], ymm0 
		i = i + 32
	ENDM

	add r10, WC_LINE * KEY_SIZE_BYTES
	mov QWORD PTR [pNext + idx0*8], r10			; update pNext[idx0]

	jmp PROCESS_2ND_KEY

	BULK_NOP 5

GET_OUTPUT_SLICE1:
	; pop a new slice from the stack
	mov r10, [free_stack-8]
	sub free_stack, 8

	mov BYTE PTR [sd], idx0b				; one byte is enough
	mov QWORD PTR [sd + 1], r10
	add sd, 9

	jmp MOVE_WC1

	; ---------------------------------------------------------------------------------------------------
	
	BULK_NOP 1

OFFLOAD2:
	mov r10, QWORD PTR [pNext + idx1*8]			; r10 = pNext[idx1] start pointer
	sub p1, WC_LINE * KEY_SIZE_BYTES
	mov QWORD PTR [stmp + idx1*8], p1			; p1 = stmp[idx1]

	test r10, SLICE_SIZE - 1					; pNext is NULL initially or aligned to slice boundary when crossing slices
	jz GET_OUTPUT_SLICE2

MOVE_WC2:
	i = 0
	while i LT WC_LINE * KEY_SIZE_BYTES
		VMOVDQU ymm0, YMMWORD PTR [p1 + i] 
		VMOVNTDQ YMMWORD PTR [r10 + i], ymm0 
		i = i + 32
	ENDM

	add r10, WC_LINE * KEY_SIZE_BYTES
	mov QWORD PTR [pNext + idx1*8], r10			; update pNext[idx1]
	jmp CONTINUE

	BULK_NOP 4

GET_OUTPUT_SLICE2:
	mov r10, [free_stack-8]
	sub free_stack, 8

	mov BYTE PTR [sd], idx1b
	mov QWORD PTR [sd + 1], r10
	add sd, 9

	jmp MOVE_WC2			
	
	; ---------------------------------------------------------------------------------------------------

OFFLOAD3:
	mov r10, QWORD PTR [pNext + idx0*8]				; r10 = pNext[idx0] start pointer
	sub p0, WC_LINE * KEY_SIZE_BYTES
	mov QWORD PTR [stmp + idx0*8], p0				; stmp[idx0] = p0

	test r10, SLICE_SIZE - 1						; pNext is NULL initially or aligned to slice boundary when crossing slices
	jz GET_OUTPUT_SLICE3

MOVE_WC3:
	i = 0
	while i LT WC_LINE * KEY_SIZE_BYTES
		VMOVDQU ymm0, YMMWORD PTR [p0 + i] 
		VMOVNTDQ YMMWORD PTR [r10 + i], ymm0 
		i = i + 32
	ENDM

	add r10, WC_LINE * KEY_SIZE_BYTES
	mov QWORD PTR [pNext + idx0*8], r10				; update pNext[buck0]
	jmp CONTINUE_FINAL

GET_OUTPUT_SLICE3:
	; pop a new slice from the stack
	mov r10, [free_stack-8]
	sub free_stack, 8

	mov BYTE PTR [sd], idx0b
	mov QWORD PTR [sd + 1], r10
	add sd, 9

	jmp MOVE_WC3

SPLIT_FUNC_NAME(asm_L0_uint) endp
WCv2L0 ENDS 

; ============================================================================================================
; WCv2a-aligned: handles L1-L2, simple prefetch logic (next block)
; ============================================================================================================
WCv2a SEGMENT align(64) EXECUTE
align 64

AsmParams2 STRUCT
	asm_slice_array		QWORD 		?
	asm_pNext			QWORD		?
	asm_digit			QWORD 		?
	asm_stmp			QWORD 		?
	asm_free_stack		QWORD 		?
	asm_sd				QWORD		?
AsmParams2 ENDS

SPLIT_FUNC_NAME(asm_L1L2_uint) proc
	; input (rcx, rdx, r8, r9) + (rax, r10, r11, xmm4, xmm5) are volatile
	; rcx = AsmParams2* ptr

	; WCv2_aligned_static uses 9 registers + r10 is tmp
	input				equ <rax>			; once per key
	pNext				equ <r8>			; once per wcLine
	digit				equ <r9>			; once per key
	stmp				equ <r14>			; once per key
	p0					equ <r12>			; once per key
	p1					equ <r15>			; once per key
	idx0				equ <r11>			; once per key
	idx1				equ <rdi>			; once per key

	; WCv2_slices: needs an additional 5 registers
	slice_array			equ <rcx>		; once per slice
	free_stack			equ <rbx>		; once per slice
	sd					equ <rbp>		; once per slice
	prefetch_distance	equ <rdx>		; once per key
	end_slice			equ <rdx>		; can be reused on the last slice

	; usused = r13, rsi; tmp usage = r10

	push rbx
	push rbp
	push rsi
	push rdi
	push r12
	push r13
	push r14
	push r15

	push rcx

	mov sd, QWORD PTR [rcx].AsmParams2.asm_sd					; once per output slice
	mov pNext, QWORD PTR [rcx].AsmParams2.asm_pNext				; once per wcLine
	mov digit, QWORD PTR [rcx].AsmParams2.asm_digit				; once per key
	mov stmp, QWORD PTR [rcx].AsmParams2.asm_stmp				; once per key
	mov free_stack, QWORD PTR [rcx].AsmParams2.asm_free_stack	; once per output slice
	; this overwrites rcx, so it must be the last parameter to be initalized
	mov slice_array, QWORD PTR [rcx].AsmParams2.asm_slice_array	; once per input slice

	; NOTE: job partitioning ensures each thread gets a full slice, so no sharing of slices
	; NOTE: input ptr in the first slice is always aligned to 2 keys, so we never cross slice boundaries on odd keys
	; based on the notes above, odd-size buckets can be detected by looking only at the end pointer

	NEXT_INPUT_SLICE:
		; 2 bottom bits of pointer: 00 = full & not last; 01 = partial & last; 02 = full & last
		mov input, QWORD PTR [slice_array]

		test input, 1
		jnz FINAL_SLICE 

		mov prefetch_distance, [slice_array + 8]			; next slice pointer
		sub prefetch_distance, input						; dist = next - cur
		add slice_array, 8

		AGAIN:
			prefetchT2 QWORD PTR [input + prefetch_distance]
	
			movzx idx0, BYTE PTR [input + digit]						
			movzx idx1, BYTE PTR [input + digit + KEY_SIZE_BYTES]		
			mov p0, QWORD PTR [stmp + idx0*8]			; p0 = stmp[buck0]
			mov p1, QWORD PTR [stmp + idx1*8]			; p1 = stmp[buck1]

			mov REG(r10), [input]						; load key0		
			add input, 2 * KEY_SIZE_BYTES
			mov [p0], REG(r10)							; save key0

			add p0, KEY_SIZE_BYTES
			test p0, WC_LINE * KEY_SIZE_BYTES - 1
			jz OFFLOAD1									; r10 is available

			; ----------------------------
		PROCESS_2ND_KEY:
			cmp idx1, idx0
			cmove p1, p0

			mov REG(r10), [input - KEY_SIZE_BYTES]			; load key1	
			mov [p1], REG(r10)								; save key1

			add p1, KEY_SIZE_BYTES
			mov QWORD PTR [stmp + idx0*8], p0				; stmp[idx0] = p0
			test p1, WC_LINE * KEY_SIZE_BYTES - 1
			jz OFFLOAD2										

			mov QWORD PTR [stmp + idx1*8], p1				; stmp[idx1] = p1

			test input, SLICE_SIZE-1
			jnz	AGAIN

		; jump the input pointer back one slice and save it into free stack
		sub input, SLICE_SIZE
		mov [free_stack], input
		add free_stack, 8

		jmp NEXT_INPUT_SLICE

FINAL_SLICE: 
	lea end_slice, [input + SLICE_SIZE]
	and end_slice, NOT (SLICE_SIZE - 1)		; clear stagger from slice
	
	test input, 2							
	cmovz end_slice, [slice_array + 8]		; partial, so load the true size from the next 8 bytes
	and input, NOT 3						; clear the bottom 2 bits

	AGAIN_FINAL:	
		movzx idx0, BYTE PTR [input + digit]						
		mov p0, QWORD PTR [stmp + idx0*8]			; p0 = stmp[buck0]

		mov REG(r10), [input]						; read key0		
		add input, KEY_SIZE_BYTES
		mov [p0], REG(r10)							; save key0

		add p0, KEY_SIZE_BYTES
		test p0, WC_LINE * KEY_SIZE_BYTES - 1
		jz OFFLOAD3

		mov QWORD PTR [stmp + idx0*8], p0				; stmp[idx0] = p0
		cmp input, end_slice
		jb	AGAIN_FINAL

RETURN:

	dec input								; round down to start of slice
	and input, NOT (SLICE_SIZE-1)
	mov [free_stack], input
	add free_stack, 8

	pop rcx
	mov QWORD PTR [rcx].AsmParams2.asm_sd, sd				; return the new sd
	mov rax, free_stack										; return the free-stack pointer

	pop r15
	pop r14
	pop r13
	pop r12
	pop rdi
	pop rsi
	pop rbp
	pop rbx

	;vzeroupper								; on Haswell and older CPUs, the histogram gets slow if mixing SSE and AVX instructions

	ret

	; ---------------------------------------------------------------------------------------------------
	BULK_NOP 6

OFFLOAD1:
	mov r10, QWORD PTR [pNext + idx0*8]			; r10 = pNext[buck0] start pointer
	sub p0, WC_LINE * KEY_SIZE_BYTES

	test r10, SLICE_SIZE - 1					; pNext is NULL initially or aligned to slice boundary when crossing slices
	jz GET_OUTPUT_SLICE1

MOVE_WC1:
	i = 0
	while i LT WC_LINE * KEY_SIZE_BYTES
		VMOVDQU ymm0, YMMWORD PTR [p0 + i] 
		VMOVNTDQ YMMWORD PTR [r10 + i], ymm0 
		i = i + 32
	ENDM

	add r10, WC_LINE * KEY_SIZE_BYTES
	mov QWORD PTR [pNext + idx0*8], r10			; update pNext[buck0]

	jmp PROCESS_2ND_KEY

	BULK_NOP 5

GET_OUTPUT_SLICE1:
	; pop a new slice from the stack
	mov r10, [free_stack-8]
	sub free_stack, 8

	mov BYTE PTR [sd], idx0b
	mov QWORD PTR [sd + 1], r10
	add sd, 9

	jmp MOVE_WC1

	; ---------------------------------------------------------------------------------------------------
	
	BULK_NOP 9

OFFLOAD2:
	mov r10, QWORD PTR [pNext + idx1*8]			; r10 = pNext[idx1] start pointer
	sub p1, WC_LINE * KEY_SIZE_BYTES
	mov QWORD PTR [stmp + idx1*8], p1			; stmp[idx1] = p1

	test r10, SLICE_SIZE - 1					; pNext is NULL initially or aligned to slice boundary when crossing slices
	jz GET_OUTPUT_SLICE2

MOVE_WC2:
	i = 0
	while i LT WC_LINE * KEY_SIZE_BYTES
		VMOVDQU ymm0, YMMWORD PTR [p1 + i] 
		VMOVNTDQ YMMWORD PTR [r10 + i], ymm0 
		i = i + 32
	ENDM

	add r10, WC_LINE * KEY_SIZE_BYTES
	mov QWORD PTR [pNext + idx1*8], r10			; update pNext[idx1]

	test input, SLICE_SIZE-1
	jnz	AGAIN
	
	; bring the input pointer to previous slice and save it into free stack
	sub input, SLICE_SIZE
	mov [free_stack], input
	add free_stack, 8

	jmp NEXT_INPUT_SLICE

	BULK_NOP 4

GET_OUTPUT_SLICE2:
	mov r10, [free_stack - 8]
	sub free_stack, 8

	; store the current slice into the dump
	mov BYTE PTR [sd], idx1b
	mov QWORD PTR [sd + 1], r10
	add sd, 9

	jmp MOVE_WC2			
	
	; ---------------------------------------------------------------------------------------------------

	BULK_NOP 17

OFFLOAD3:
	mov r10, QWORD PTR [pNext + idx0*8]				; r10 = pNext[buck0] start pointer
	sub p0, WC_LINE * KEY_SIZE_BYTES
	mov QWORD PTR [stmp + idx0*8], p0				; stmp[buck1] = p1

	test r10, SLICE_SIZE - 1						; pNext is NULL initially or aligned to slice boundary when crossing slices
	jz GET_OUTPUT_SLICE3

MOVE_WC3:
	i = 0
	while i LT WC_LINE * KEY_SIZE_BYTES
		VMOVDQU ymm0, YMMWORD PTR [p0 + i] 
		VMOVNTDQ YMMWORD PTR [r10 + i], ymm0 
		i = i + 32
	ENDM

	add r10, WC_LINE * KEY_SIZE_BYTES
	mov QWORD PTR [pNext + idx0*8], r10				; update pNext[buck0]

	cmp input, end_slice
	jb	AGAIN_FINAL

	jmp RETURN

	BULK_NOP 5

GET_OUTPUT_SLICE3:
	; pop a new slice from the stack
	mov r10, [free_stack - 8]
	sub free_stack, 8

	mov BYTE PTR [sd], idx0b
	mov QWORD PTR [sd + 1], r10
	add sd, 9
	
	jmp MOVE_WC3

SPLIT_FUNC_NAME(asm_L1L2_uint) endp
WCv2a ENDS 

; =========================================================================
; tmp bucket cleanup (L0-L2)
; =========================================================================
s51 SEGMENT align(64) EXECUTE
align 64
	BULK_NOP 9
SPLIT_FUNC_NAME(asm_cleanup_uint) proc
	; input (rcx, rdx, r8, r9) + (rax, r10, r11, xmm4, xmm5) are volatile
	; rcx = pNext[]
	; rdx = stmp
	; r8 = free_stack
	; r9 = &sliceDump 

	pNext		equ <rcx>
	stmp		equ <rdx>
	free_stack	equ <r8>
	sd			equ <r9>
	idx			equ <rax>
	idxb		equ <al>
	src			equ <r12>
	dest		equ <r10>
	bytes		equ <r13>
	end_src		equ <r11>

	; realStagger is stmp + 8*256

	push rbx
	push r12
	push r13
	push rbp

	push sd
	mov sd, QWORD PTR [sd]							; load sliceDump 

	xor idx, idx									; rax = current bucket idx

LOOP_TOP:
	mov dest, QWORD PTR [pNext + idx*8]			; r10 = pNext[rax] start pointer in RAM; have to do this here before jmp to BYPASS
	xor rbp, rbp
	test dest, dest											; if dest == 0, this is the first offload, so grab stagger
	cmovz bp, WORD PTR [stmp + 256*8 + idx*2]				; load stagger 
	mov src, QWORD PTR [stmp + idx*8]						; stmp[idx] pointer
	mov bytes, src
	and bytes, WC_LINE * KEY_SIZE_BYTES - 1				; number of bytes to copy

	cmp rbp, bytes											; if bytes to move equals stagger, the bucket is empty
	je BYPASS

	sub src, bytes											; reset src to start of bucket

COPY:	
	test dest, SLICE_SIZE - 1								; pNext is NULL initially or aligned to slice boundary when crossing slices
	jz GET_OUTPUT_SLICE

CONTINUE_OFFLOAD:
	vmovdqu ymm0, YMMWORD PTR [src]
	VMOVNTDQ YMMWORD PTR [dest], ymm0
	add dest, 32
	add src, 32
	sub bytes, 32

	test bytes, bytes
	jg COPY
		
	add dest, bytes									; roll back by the overshoot (bytes <= 0, so adding is subtraction)
	mov QWORD PTR [pNext + idx*8], dest				; update pNext[idx]

BYPASS:
	inc idx											; next bucket
	cmp idx, 256
	jb LOOP_TOP

	pop rax											; pointer to sliceDump
	mov QWORD PTR [rax], sd							; update sliceDump 
	mov rax, free_stack								; return the stack

	pop rbp
	pop r13
	pop r12
	pop rbx

	ret

GET_OUTPUT_SLICE:
	; pop a new one from the stack
	sub free_stack, 8
	mov dest, [free_stack]

	; store the current slice into the slice database
	mov BYTE PTR [sd], idxb
	mov QWORD PTR [sd + 1], dest
	add sd, 9

	jmp CONTINUE_OFFLOAD

SPLIT_FUNC_NAME(asm_cleanup_uint) endp
s51 ENDS

; =========================================================================
; tmp bucket cleanup (L4)
; =========================================================================
s52 SEGMENT align(64) EXECUTE
align 64
	nop DWORD PTR [rax+300]		; 7 bytes

M_GET_OUTPUT_SLICE MACRO idx
LOCAL BAIL_OUT
	mov rsi, QWORD PTR [sd + idx*8]				; read the database 
	mov r10, QWORD PTR [rsi]

	test r10, r10								; a non-zero slice in the database indicates pre-assigned slices
	jnz BAIL_OUT

	; pop a new slice from the stack
	mov r10, [free_stack-8]
	sub free_stack, 8

	; store the new slice into the slice database
	mov QWORD PTR [rsi], r10

BAIL_OUT:
	add rsi, 8
	mov [sd + idx*8], rsi
ENDM


SPLIT_FUNC_NAME(asm_cleanup_L4_uint) proc
	; input (rcx, rdx, r8, r9) + (rax, r10, r11, xmm4, xmm5) are volatile
	; rcx = pNext[]
	; rdx = stmp
	; r8 = free_stack
	; r9 = sd

	pNext		equ <rcx>
	stmp		equ <rdx>
	free_stack	equ <r8>
	sd			equ <r9>
	idx			equ <rax>
	end_p		equ <rbx>
	start_p		equ <r12>
	
	; additonally, r10-r11, rsi are tmp
	; realStagger is stmp + 8*256

	push rbx
	push r12
	push rsi
	push rbp

	xor idx, idx									; rax = current bucket idx

LOOP_TOP:
	mov r10, QWORD PTR [pNext + idx*8]				; r10 = pNext[rax] start pointer in RAM; have to do this here before jmp to BYPASS
	mov end_p, QWORD PTR [stmp + idx*8]				; stmp[idx]

	mov start_p, end_p										; end of tmp bucket
	and start_p, NOT (WC_LINE * KEY_SIZE_BYTES - 1)		; start of tmp bucket
	mov QWORD PTR [stmp + idx*8], start_p					; empty tmp bucket size -- not needed!

	test r10, r10											; if r10==0, this is the first offload, so add stagger
	jnz NO_ADJUSTMENT

	movzx rbp, WORD PTR [stmp + 256*8 + idx*2]				; stagger for L4 in bytes
	add start_p, rbp

NO_ADJUSTMENT:
	cmp start_p, end_p										; empty bucket
	je BYPASS

COPY:	
	test r10, SLICE_SIZE - 1						; except for L4, pNext is NULL initially or aligned to slice boundary when crossing slices
	jz GET_OUTPUT_SLICE

CONTINUE_OFFLOAD:
	mov REG(r11), [start_p]
	mov [r10], REG(r11)
	add start_p, KEY_SIZE_BYTES
	add r10, KEY_SIZE_BYTES							

	cmp start_p, end_p
	jb COPY

	mov QWORD PTR [pNext + idx*8], r10				; update pNext[idx]

BYPASS:
	inc idx											; next bucket
	cmp idx, 256
	jb LOOP_TOP

	mov rax, free_stack								; return the stack

	pop rbp
	pop rsi
	pop r12
	pop rbx

	ret

GET_OUTPUT_SLICE:
	M_GET_OUTPUT_SLICE idx
	jmp CONTINUE_OFFLOAD

SPLIT_FUNC_NAME(asm_cleanup_L4_uint) endp
s52 ENDS
       
; ============================================================================================================
; WCv2-unalignedB:	handles L4
; ============================================================================================================
WCv2uB SEGMENT align(64) EXECUTE
align 64
	BULK_NOP 33

SPLIT_FUNC_NAME(asm_L4_uint) proc
	; input (rcx, rdx, r8, r9) + (rax, r10, r11, xmm4, xmm5) are volatile
	; rcx = AsmParams2* ptr

	; In the Microsoft x64 calling convention, it is the caller's responsibility to allocate 32 
	; bytes of "shadow space" on the stack right before calling the function (regardless 
	; of the actual number of parameters used), and to pop the stack after the call. 
	; The shadow space is used to spill RCX, RDX, R8, and R9, but must be made available to all 
	; functions, even those with fewer than four parameters.
	; https://en.wikipedia.org/wiki/X86_calling_conventions#Microsoft_x64_calling_convention

	; WCv2_aligned_static uses 9 registers + r10 is tmp
	input				equ <rax>			; once per key
	pNext				equ <r8>			; once per wcLine
	digit				equ <r9>			; once per key
	stmp				equ <r14>			; once per key
	p0					equ <r12>			; once per key
	p1					equ <r15>			; once per key
	idx0				equ <r11>			; once per key
	idx1				equ <rdi>			; once per key

	; WCv2_slices: needs an additional 5 registers + rsi is tmp
	slice_array			equ <rcx>			; once per slice
	free_stack			equ <rbx>			; once per slice
	sd					equ <rbp>			; once per slice
	prefetch_distance	equ <rdx>			; once per key
	end_slice			equ <rdx>			; can be reused on the last slice

	free_reg0 TEXTEQU <r10>					; needed for key movement (=key or pNext[idx])
	free_reg1 TEXTEQU <rsi>		

	; usused = r13

	push rbx
	push rbp
	push rsi
	push rdi
	push r12
	push r13
	push r14
	push r15

	mov sd, QWORD PTR [rcx].AsmParams2.asm_sd					; once per output slice
	mov pNext, QWORD PTR [rcx].AsmParams2.asm_pNext				; once per wcLine
	mov digit, QWORD PTR [rcx].AsmParams2.asm_digit				; once per key
	mov stmp, QWORD PTR [rcx].AsmParams2.asm_stmp				; once per key
	mov free_stack, QWORD PTR [rcx].AsmParams2.asm_free_stack	; once per output slice
	; this overwrites rcx, so it must be the last parameter to be initalized
	mov slice_array, QWORD PTR [rcx].AsmParams2.asm_slice_array	; once per input slice

	; NOTE: job partitioning ensures each thread gets a full slice, so no sharing of slices
	; NOTE: input ptr in the first slice is always aligned to 2 keys, so we never cross slice boundaries on odd keys
	; based on the notes above, odd-size buckets can be detected by looking only at the end pointer

	NEXT_INPUT_SLICE:
		; 2 bottom bits of pointer: 00 = full & not last; 01 = partial & last; 02 = full & last
		mov input, QWORD PTR [slice_array]

		test input, 1
		jnz FINAL_SLICE 

		mov prefetch_distance, [slice_array + 8]
		sub prefetch_distance, input
		add slice_array, 8

		AGAIN:
			prefetchT2 QWORD PTR [input + prefetch_distance]
	
			movzx idx0, BYTE PTR [input + digit]						
			movzx idx1, BYTE PTR [input + digit + KEY_SIZE_BYTES]		
			mov p0, QWORD PTR [stmp + idx0*8]			; stmp[buck0] = p0
			mov p1, QWORD PTR [stmp + idx1*8]			; stmp[buck1]

			mov REG(r10), [input]						; r10 = key0		
			add input, 2*KEY_SIZE_BYTES
			mov [p0], REG(r10)							; save key0

			add p0, KEY_SIZE_BYTES
			test p0, WC_LINE * KEY_SIZE_BYTES - 1
			jz OFFLOAD1									; r10 is available

			; ----------------------------
		PROCESS_2ND_KEY:
			cmp idx1, idx0
			cmove p1, p0

			mov REG(r10), [input - KEY_SIZE_BYTES]		; rax = key1	
			mov [p1], REG(r10)							; save key1

			add p1, KEY_SIZE_BYTES
			mov QWORD PTR [stmp + idx0*8], p0				; stmp[buck0] = p0
			test p1, WC_LINE * KEY_SIZE_BYTES - 1
			jz OFFLOAD2										; r10, r11 are available

			mov QWORD PTR [stmp + idx1*8], p1				; stmp[buck1] = p1

		CONTINUE:
			test input, SLICE_SIZE-1
			jnz	AGAIN

		; round down the input pointer and save slice into free stack
		sub input, SLICE_SIZE
		mov [free_stack], input
		add free_stack, 8

		; unconditional jump
		jmp NEXT_INPUT_SLICE

FINAL_SLICE: 
	lea end_slice, [input + SLICE_SIZE]
	and end_slice, NOT (SLICE_SIZE - 1)		; clear stagger from slice
	
	test input, 2							
	cmovz end_slice, [slice_array + 8]		; partial

	and input, NOT 3						; the final slice is offset by 1 or 3 from the correct start

	AGAIN_FINAL:	
		movzx idx0, BYTE PTR [input + digit]						
		mov p0, QWORD PTR [stmp + idx0*8]			; stmp[buck0] = p0

		mov REG(r10), [input]						; r10 = key0		
		add input, KEY_SIZE_BYTES
		mov [p0], REG(r10)							; save key0

		add p0, KEY_SIZE_BYTES
		test p0, WC_LINE * KEY_SIZE_BYTES - 1
		jz OFFLOAD3
	
		mov QWORD PTR [stmp + idx0*8], p0				; stmp[buck1] = p1

	CONTINUE_FINAL:
		cmp input, end_slice
		jb	AGAIN_FINAL

	dec input								; round down to start of slice
	and input, NOT (SLICE_SIZE-1)
	mov [free_stack], input
	add free_stack, 8

	mov rax, free_stack						; return the free-stack pointer

	pop r15
	pop r14
	pop r13
	pop r12
	pop rdi
	pop rsi
	pop rbp
	pop rbx

	;vzeroupper								; on Haswell and older CPUs, the histogram gets slow if mixing SSE and AVX instructions

	ret

; ---------------------------------------------
NEW_SLICE1: 
	M_GET_OUTPUT_SLICE idx0
	jmp FULL_OFFLOAD1

	BULK_NOP 14

OFFLOAD1:
	sub p0, WC_LINE * KEY_SIZE_BYTES
	mov r10, QWORD PTR [pNext + idx0*8]			; r10 = pNext[idx0] start pointer

	test r10, r10
	jz FIRST_VISIT1

	; ====================================
	; 1) x = r10 & (SLICE-1)
	; 2) if (x==0) jmp XYZ
	mov r13, r10											
	and r13, SLICE_SIZE - 1									; bytes occupied in current slice
	jz NEW_SLICE1							

	cmp r13, SLICE_SIZE - WC_LINE*KEY_SIZE_BYTES			; not enough space for a full offload?
	ja TWOLOOPS1

FULL_OFFLOAD1:
	i = 0
	while i LT WC_LINE * KEY_SIZE_BYTES
		VMOVDQU ymm0, YMMWORD PTR [p0 + i] 
		VMOVNTDQ YMMWORD PTR [r10 + i], ymm0 
		i = i + 32
	ENDM
	
	add r10, WC_LINE * KEY_SIZE_BYTES
	mov QWORD PTR [pNext + idx0*8], r10			; update pNext[buck0]
	jmp PROCESS_2ND_KEY

	; -----------------------------------------
	nop 
TWOLOOPS1:
	; 3) y = min(SLICE - x, CACHE_LINE) = number of bytes to move
	dec r13										
	xor r13, SLICE_SIZE-1						; SLICE-x = (x-1)^(SLICE-1) for power-of-2 slices
	lea r13, [p0 + r13]							; partial offload ends here

	; 4) loop from p0 to p0+y
	LOOP1A:
		vmovdqu ymm0, YMMWORD PTR [p0]
		vmovntdq YMMWORD PTR [r10], ymm0
		add p0, 32
		add r10, 32
		cmp p0, r13
		jb LOOP1A

		M_GET_OUTPUT_SLICE idx0

	LOOP1B:
		vmovdqu ymm0, YMMWORD PTR [p0]
		vmovntdq YMMWORD PTR [r10], ymm0
		add p0, 32
		add r10, 32
		test p0, WC_LINE * KEY_SIZE_BYTES - 1
		jnz LOOP1B

	sub p0, WC_LINE * KEY_SIZE_BYTES
	mov QWORD PTR [pNext + idx0*8], r10			; update pNext[buck0]
	jmp PROCESS_2ND_KEY

	BULK_NOP 2

FIRST_VISIT1:
	movzx rsi, WORD PTR [stmp + 256*8 + idx0*2]		; get stagger
	add p0, rsi

	FIRST_VISIT_LOOP1:
		test r10, SLICE_SIZE - 1
		jnz WERE_GOOD_FIRST
		
		M_GET_OUTPUT_SLICE idx0

	WERE_GOOD_FIRST:
		mov REG(rsi), [p0]
		mov [r10], REG(rsi)
		add p0, KEY_SIZE_BYTES
		add r10, KEY_SIZE_BYTES
		test p0, WC_LINE * KEY_SIZE_BYTES - 1
		jnz FIRST_VISIT_LOOP1

	sub p0,  WC_LINE * KEY_SIZE_BYTES
	mov QWORD PTR [pNext + idx0*8], r10					; update pNext[buck0]
	jmp PROCESS_2ND_KEY									

	; ---------------------------------------------------------------------------------------------------
	
NEW_SLICE2: 
	M_GET_OUTPUT_SLICE idx1
	jmp FULL_OFFLOAD2

	BULK_NOP 9

OFFLOAD2:
	sub p1, WC_LINE * KEY_SIZE_BYTES
	mov QWORD PTR [stmp + idx1*8], p1				; stmp[buck1] = p1
	mov r10, QWORD PTR [pNext + idx1*8]			; r10 = pNext[buck1] start pointer

	test r10, r10									; stagger != 0, first visit to this bucket that needs special handling
	jz FIRST_VISIT2

	; =======================================================
	mov r13, r10											
	and r13, SLICE_SIZE - 1									; bytes occupied in current slice
	jz NEW_SLICE2

	cmp r13, SLICE_SIZE - WC_LINE*KEY_SIZE_BYTES			; not enough space for a full offload?
	ja TWOLOOPS2

FULL_OFFLOAD2:
	i = 0
	while i LT WC_LINE * KEY_SIZE_BYTES
		VMOVDQU ymm0, YMMWORD PTR [p1 + i] 
		VMOVNTDQ YMMWORD PTR [r10 + i], ymm0 
		i = i + 32
	ENDM
	
	add r10, WC_LINE * KEY_SIZE_BYTES
	mov QWORD PTR [pNext + idx1*8], r10			; update pNext[buck1]
	jmp CONTINUE

	; -----------------------------------------
	BULK_NOP 12
TWOLOOPS2:
	; 3) y = min(SLICE - x, CACHE_LINE) = number of bytes to move
	dec r13										; x = ptr & (SLICE-1)
	xor r13, SLICE_SIZE-1						; SLICE-x = (x-1)^(SLICE-1) for power-of-2 slices
	lea r13, [p1 + r13]							; partial offload ends here

	; 4) loop from p0 to p0+y
	LOOP2A:
		vmovdqu ymm0, YMMWORD PTR [p1]
		vmovntdq YMMWORD PTR [r10], ymm0
		add p1, 32
		add r10, 32
		cmp p1, r13
		jb LOOP2A

	M_GET_OUTPUT_SLICE idx1

	LOOP2B:
		vmovdqu ymm0, YMMWORD PTR [p1]				; the rest fits into the slice
		vmovntdq YMMWORD PTR [r10], ymm0
		add p1, 32
		add r10, 32
		test p1, WC_LINE * KEY_SIZE_BYTES - 1
		jnz LOOP2B

	mov QWORD PTR [pNext + idx1*8], r10				; update pNext[buck1]
	jmp CONTINUE

	BULK_NOP 10

FIRST_VISIT2:
	movzx rsi, WORD PTR [stmp + 256*8 + idx1*2]			; get stagger
	add p1, rsi											; start from this offset

	FIRST_VISIT_LOOP2:
		test r10, SLICE_SIZE - 1
		jnz WERE_GOOD_FIRST2

		M_GET_OUTPUT_SLICE idx1

	WERE_GOOD_FIRST2:
		mov REG(rsi), [p1]
		mov [r10], REG(rsi)
		add p1, KEY_SIZE_BYTES
		add r10, KEY_SIZE_BYTES
		test p1, WC_LINE * KEY_SIZE_BYTES - 1
		jnz FIRST_VISIT_LOOP2

	mov QWORD PTR [pNext + idx1*8], r10			; update pNext[buck1]
	jmp CONTINUE
	
	; ---------------------------------------------------------------------------------------------------
	BULK_NOP 3

OFFLOAD3:
	sub p0, WC_LINE * KEY_SIZE_BYTES
	mov QWORD PTR [stmp + idx0*8], p0				; stmp[buck1] = p1
	mov r10, QWORD PTR [pNext + idx0*8]				; r10 = pNext[buck0] start pointer
	
	test r10, r10
	jz FIRST_VISIT3

	MORE3:
		test r10, SLICE_SIZE - 1					; pNext is NULL initially or aligned to slice boundary when crossing slices
		jnz WERE_GOOD3

		M_GET_OUTPUT_SLICE idx0

	WERE_GOOD3:
		vmovdqu ymm0, YMMWORD PTR [p0]
		vmovntdq YMMWORD PTR [r10], ymm0
		add p0, 32
		add r10, 32
		test p0, WC_LINE * KEY_SIZE_BYTES - 1
		jnz MORE3

	mov QWORD PTR [pNext + idx0*8], r10				; update pNext[buck0]
	jmp CONTINUE_FINAL

	BULK_NOP 5

FIRST_VISIT3:
	movzx rsi, WORD PTR [stmp + 256*8 + idx0*2]			; get stagger
	add p0, rsi											; start from this offset

	FIRST_VISIT_LOOP3:
		test r10, SLICE_SIZE - 1
		jnz WERE_GOOD_FIRST3

		M_GET_OUTPUT_SLICE idx0

	WERE_GOOD_FIRST3:
		mov REG(rsi), [p0]
		mov [r10], REG(rsi)
		add p0, KEY_SIZE_BYTES
		add r10, KEY_SIZE_BYTES
		test p0, WC_LINE * KEY_SIZE_BYTES - 1
		jnz FIRST_VISIT_LOOP3

	mov QWORD PTR [pNext + idx0*8], r10					; update pNext[buck0]
	jmp CONTINUE_FINAL


SPLIT_FUNC_NAME(asm_L4_uint) endp
WCv2uB ENDS 