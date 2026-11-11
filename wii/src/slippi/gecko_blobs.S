/* Slippi's Gecko codesets (project-slippi/slippi-ssbm-asm, built:
 * wii/slippi/gecko), in the program: melee_gecko_NAME and its
 * size, melee_gecko_NAME_size. */

	.section .rodata.melee_gecko,"a"
	.balign 32
	.globl melee_gecko_core
melee_gecko_core:
	.incbin "g_core.bin"
melee_gecko_core_end:
	.balign 4
	.globl melee_gecko_core_size
melee_gecko_core_size:
	.long melee_gecko_core_end - melee_gecko_core
	.balign 32
	.globl melee_gecko_core_porta
melee_gecko_core_porta:
	.incbin "g_core_porta.bin"
melee_gecko_core_porta_end:
	.balign 4
	.globl melee_gecko_core_porta_size
melee_gecko_core_porta_size:
	.long melee_gecko_core_porta_end - melee_gecko_core_porta
	.balign 32
	.globl melee_gecko_crash_output
melee_gecko_crash_output:
	.incbin "g_crash_output.bin"
melee_gecko_crash_output_end:
	.balign 4
	.globl melee_gecko_crash_output_size
melee_gecko_crash_output_size:
	.long melee_gecko_crash_output_end - melee_gecko_crash_output
	.balign 32
	.globl melee_gecko_gameplay_both
melee_gecko_gameplay_both:
	.incbin "g_gameplay_both.bin"
melee_gecko_gameplay_both_end:
	.balign 4
	.globl melee_gecko_gameplay_both_size
melee_gecko_gameplay_both_size:
	.long melee_gecko_gameplay_both_end - melee_gecko_gameplay_both
	.balign 32
	.globl melee_gecko_gameplay_lgl
melee_gecko_gameplay_lgl:
	.incbin "g_gameplay_lgl.bin"
melee_gecko_gameplay_lgl_end:
	.balign 4
	.globl melee_gecko_gameplay_lgl_size
melee_gecko_gameplay_lgl_size:
	.long melee_gecko_gameplay_lgl_end - melee_gecko_gameplay_lgl
	.balign 32
	.globl melee_gecko_gameplay_wobbling
melee_gecko_gameplay_wobbling:
	.incbin "g_gameplay_wobbling.bin"
melee_gecko_gameplay_wobbling_end:
	.balign 4
	.globl melee_gecko_gameplay_wobbling_size
melee_gecko_gameplay_wobbling_size:
	.long melee_gecko_gameplay_wobbling_end - melee_gecko_gameplay_wobbling
	.balign 32
	.globl melee_gecko_lag_pd
melee_gecko_lag_pd:
	.incbin "g_lag_pd.bin"
melee_gecko_lag_pd_end:
	.balign 4
	.globl melee_gecko_lag_pd_size
melee_gecko_lag_pd_size:
	.long melee_gecko_lag_pd_end - melee_gecko_lag_pd
	.balign 32
	.globl melee_gecko_lag_pdhalfvb
melee_gecko_lag_pdhalfvb:
	.incbin "g_lag_pdhalfvb.bin"
melee_gecko_lag_pdhalfvb_end:
	.balign 4
	.globl melee_gecko_lag_pdhalfvb_size
melee_gecko_lag_pdhalfvb_size:
	.long melee_gecko_lag_pdhalfvb_end - melee_gecko_lag_pdhalfvb
	.balign 32
	.globl melee_gecko_mods_friendlies
melee_gecko_mods_friendlies:
	.incbin "g_mods_friendlies.bin"
melee_gecko_mods_friendlies_end:
	.balign 4
	.globl melee_gecko_mods_friendlies_size
melee_gecko_mods_friendlies_size:
	.long melee_gecko_mods_friendlies_end - melee_gecko_mods_friendlies
	.balign 32
	.globl melee_gecko_mods_stealth
melee_gecko_mods_stealth:
	.incbin "g_mods_stealth.bin"
melee_gecko_mods_stealth_end:
	.balign 4
	.globl melee_gecko_mods_stealth_size
melee_gecko_mods_stealth_size:
	.long melee_gecko_mods_stealth_end - melee_gecko_mods_stealth
	.balign 32
	.globl melee_gecko_mods_tournament
melee_gecko_mods_tournament:
	.incbin "g_mods_tournament.bin"
melee_gecko_mods_tournament_end:
	.balign 4
	.globl melee_gecko_mods_tournament_size
melee_gecko_mods_tournament_size:
	.long melee_gecko_mods_tournament_end - melee_gecko_mods_tournament
	.balign 32
	.globl melee_gecko_online_boot
melee_gecko_online_boot:
	.incbin "g_online_boot.bin"
melee_gecko_online_boot_end:
	.balign 4
	.globl melee_gecko_online_boot_size
melee_gecko_online_boot_size:
	.long melee_gecko_online_boot_end - melee_gecko_online_boot
	.balign 32
	.globl melee_gecko_pal
melee_gecko_pal:
	.incbin "g_pal.bin"
melee_gecko_pal_end:
	.balign 4
	.globl melee_gecko_pal_size
melee_gecko_pal_size:
	.long melee_gecko_pal_end - melee_gecko_pal
	.balign 32
	.globl melee_gecko_safety
melee_gecko_safety:
	.incbin "g_safety.bin"
melee_gecko_safety_end:
	.balign 4
	.globl melee_gecko_safety_size
melee_gecko_safety_size:
	.long melee_gecko_safety_end - melee_gecko_safety
	.balign 32
	.globl melee_gecko_screen_wide
melee_gecko_screen_wide:
	.incbin "g_screen_wide.bin"
melee_gecko_screen_wide_end:
	.balign 4
	.globl melee_gecko_screen_wide_size
melee_gecko_screen_wide_size:
	.long melee_gecko_screen_wide_end - melee_gecko_screen_wide
	.balign 32
	.globl melee_gecko_screen_wide_shutters
melee_gecko_screen_wide_shutters:
	.incbin "g_screen_wide_shutters.bin"
melee_gecko_screen_wide_shutters_end:
	.balign 4
	.globl melee_gecko_screen_wide_shutters_size
melee_gecko_screen_wide_shutters_size:
	.long melee_gecko_screen_wide_shutters_end - melee_gecko_screen_wide_shutters
	.balign 32
	.globl melee_gecko_stages_all
melee_gecko_stages_all:
	.incbin "g_stages_all.bin"
melee_gecko_stages_all_end:
	.balign 4
	.globl melee_gecko_stages_all_size
melee_gecko_stages_all_size:
	.long melee_gecko_stages_all_end - melee_gecko_stages_all
	.balign 32
	.globl melee_gecko_stages_stadium
melee_gecko_stages_stadium:
	.incbin "g_stages_stadium.bin"
melee_gecko_stages_stadium_end:
	.balign 4
	.globl melee_gecko_stages_stadium_size
melee_gecko_stages_stadium_size:
	.long melee_gecko_stages_stadium_end - melee_gecko_stages_stadium
	.balign 32
	.globl melee_gecko_ucf
melee_gecko_ucf:
	.incbin "g_ucf.bin"
melee_gecko_ucf_end:
	.balign 4
	.globl melee_gecko_ucf_size
melee_gecko_ucf_size:
	.long melee_gecko_ucf_end - melee_gecko_ucf
	.balign 32
	.globl melee_gecko_ucf_084
melee_gecko_ucf_084:
	.incbin "g_ucf_084.bin"
melee_gecko_ucf_084_end:
	.balign 4
	.globl melee_gecko_ucf_084_size
melee_gecko_ucf_084_size:
	.long melee_gecko_ucf_084_end - melee_gecko_ucf_084
	.balign 32
	.globl melee_gecko_ucf_084_stealth
melee_gecko_ucf_084_stealth:
	.incbin "g_ucf_084_stealth.bin"
melee_gecko_ucf_084_stealth_end:
	.balign 4
	.globl melee_gecko_ucf_084_stealth_size
melee_gecko_ucf_084_stealth_size:
	.long melee_gecko_ucf_084_stealth_end - melee_gecko_ucf_084_stealth
	.balign 32
	.globl melee_gecko_ucf_stealth
melee_gecko_ucf_stealth:
	.incbin "g_ucf_stealth.bin"
melee_gecko_ucf_stealth_end:
	.balign 4
	.globl melee_gecko_ucf_stealth_size
melee_gecko_ucf_stealth_size:
	.long melee_gecko_ucf_stealth_end - melee_gecko_ucf_stealth
