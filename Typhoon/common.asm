; @file  common.asm
; @brief combined assembly functions
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

.data 

extern min_stack:QWORD

; MASM operators https://msdn.microsoft.com/en-us/library/94b6khh4.aspx

SLICE_SIZE = (1 SHL 14)

.code
 
REG32 MACRO x
IFIDNI <x>, <rax>
	exitm<eax>
ENDIF
IFIDNI <x>, <rbx>
	exitm<ebx>
ENDIF
IFIDNI <x>, <rcx>
	exitm<ecx>
ENDIF
IFIDNI <x>, <rdx>
	exitm<edx>
ENDIF
IFIDNI <x>, <rsi>
	exitm<esi>
ENDIF
IFIDNI <x>, <rdi>
	exitm<edi>
ENDIF
	exitm <&x&d>
ENDM

REG64 MACRO x
	exitm<x>
ENDM

; decide which scalar registers we'll be using (this will hold min(KV, 8) bytes)
REG MACRO x
    IF KEY_SIZE_BYTES GE 8
        exitm<REG64(x)>
    ELSEIF KEY_SIZE_BYTES EQ 4
        exitm<REG32(x)>
    ENDIF
ENDM

SPLIT_FUNC_NAME MACRO x
    local tmp
    tmp CATSTR <x>, %KEY_SIZE_BITS, <_WC>, %WC_LINE_BITS
    exitm<tmp>
ENDM

HIST_FUNC_NAME MACRO x
    local tmp
    tmp CATSTR <x>, %KEY_SIZE_BITS, <_SIMD>, %SSE_HIST_EXTRACT, <_PREF>, %PREFETCH_ALL_LEVEL
    exitm<tmp>
ENDM

; ---------------------------------
BULK_NOP MACRO x
	i = 0
	WHILE i LT x
		nop
		i=i+1
	ENDM
ENDM

asm_get_slice_size proc
		mov rax, SLICE_SIZE
		ret
asm_get_slice_size endp

KEY_SIZE_POWER = 2
WC_LINE_BITS = 4
while WC_LINE_BITS LE 11
	include split.inc
	WC_LINE_BITS = WC_LINE_BITS + 1
ENDM

KEY_SIZE_POWER = 3
WC_LINE_BITS = 4
while WC_LINE_BITS LE 11
    include split.inc
    WC_LINE_BITS = WC_LINE_BITS + 1
ENDM

KEY_SIZE_POWER = 2

KEY_SIZE_POWER = 2
SSE_HIST_EXTRACT = 0
PREFETCH_ALL_LEVEL = 0

while SSE_HIST_EXTRACT LE 1
	include hist.inc
	SSE_HIST_EXTRACT = SSE_HIST_EXTRACT + 1
ENDM

SSE_HIST_EXTRACT = 0
PREFETCH_ALL_LEVEL = 1
while SSE_HIST_EXTRACT LE 1
	include hist.inc
	SSE_HIST_EXTRACT = SSE_HIST_EXTRACT + 1
ENDM

KEY_SIZE_POWER = 3
SSE_HIST_EXTRACT = 0
PREFETCH_ALL_LEVEL = 0

while SSE_HIST_EXTRACT LE 1
	include hist.inc
	SSE_HIST_EXTRACT = SSE_HIST_EXTRACT + 1
ENDM

SSE_HIST_EXTRACT = 0
PREFETCH_ALL_LEVEL = 1
while SSE_HIST_EXTRACT LE 1
	include hist.inc
	SSE_HIST_EXTRACT = SSE_HIST_EXTRACT + 1
ENDM

end