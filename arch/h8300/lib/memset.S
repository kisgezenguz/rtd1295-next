/* SPDX-License-Identifier: GPL-2.0 */
/* memset.S */

#include <asm/linkage.h>

#if defined(CONFIG_CPU_H8300H)
	.h8300h
#endif
#if defined(CONFIG_CPU_H8S)
	.h8300s
#endif
	.text

.global	memset
.global	clear_user

;;void *memset(*ptr, int c, size_t count)
;; ptr = er0
;; c   = er1(r1l)
;; count = er2
memset:
	mov.l	er4,@-sp
	mov.l	er0,er4
	btst	#0,r4l
	beq	2f

	;; odd address
1:
	mov.b	r1l,@er4
	adds	#1,er4
	dec.l	#1,er2
	beq	6f

	;; even address
2:
	mov.l	er2,er3
	cmp.l	#4,er2
	blo	4f
	;; count>=4 -> count/4
#if defined(CONFIG_CPU_H8300H)
	shlr.l	er2
	shlr.l	er2
#endif
#if defined(CONFIG_CPU_H8S)
	shlr.l	#2,er2
#endif
	;; byte -> long
	mov.b	r1l,r1h
	mov.w	r1,e1
3:
	mov.l	er1,@er4
	adds	#4,er4
	dec.l	#1,er2
	bne	3b
4:
	;; count % 4
	and.b	#3,r3l
	beq	6f
5:
	mov.b	r1l,@er4
	adds	#1,er4
	dec.b	r3l
	bne	5b
6:
	mov.l	@sp+,er4
	rts

clear_user:
	mov.l	er1, er2
	sub.l	er1, er1
	bra	memset

	.end
