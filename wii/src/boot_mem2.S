/* The DOL's entry: the port's code and data that run in MEM2 are loaded in
 * MEM1 (wii/tools/orig_image.py: MEM1 is the game's, as on the GameCube, and a
 * loader then needs nothing but MEM1); this copies them up, makes the
 * instruction cache see them, and starts libogc (__app_start). Runs before
 * anything else: no stack, no small data. */

	.section .melee_boot,"ax",@progbits
	.balign 4
	.globl melee_boot
melee_boot:
	/* the SDK's data the DSP reads: up to the top of MEM1, in memory (the
	 * DSP's DMA reads past the cache) */
	lis	3, __dsp_vma_start@ha
	addi	3, 3, __dsp_vma_start@l
	lis	4, __dsp_load_start@ha
	addi	4, 4, __dsp_load_start@l
	lis	5, __dsp_load_end@ha
	addi	5, 5, __dsp_load_end@l
	subf	5, 4, 5
	addi	5, 5, 3
	srwi	6, 5, 2
	mtctr	6
	addi	3, 3, -4
	addi	4, 4, -4
3:	lwzu	7, 4(4)
	stwu	7, 4(3)
	bdnz	3b
	lis	3, __dsp_vma_start@ha
	addi	3, 3, __dsp_vma_start@l
	addi	5, 5, 31
	srwi	6, 5, 5
	mtctr	6
4:	dcbst	0, 3
	addi	3, 3, 32
	bdnz	4b
	sync

	lis	3, __port_vma_start@ha
	addi	3, 3, __port_vma_start@l
	lis	4, __port_load_start@ha
	addi	4, 4, __port_load_start@l
	lis	5, __port_load_end@ha
	addi	5, 5, __port_load_end@l
	subf	5, 4, 5
	addi	5, 5, 3
	srwi	6, 5, 2
	mtctr	6
	addi	3, 3, -4
	addi	4, 4, -4
1:	lwzu	7, 4(4)
	stwu	7, 4(3)
	bdnz	1b

	lis	3, __port_vma_start@ha
	addi	3, 3, __port_vma_start@l
	addi	5, 5, 31
	srwi	6, 5, 5
	mtctr	6
2:	dcbst	0, 3
	sync
	icbi	0, 3
	addi	3, 3, 32
	bdnz	2b
	sync
	isync

	lis	12, __app_start@ha
	addi	12, 12, __app_start@l
	mtctr	12
	bctr
