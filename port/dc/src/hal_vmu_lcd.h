/* VMU LCD character display (hal_vmu_lcd.c): the game's own Klonoa sprite frames during gameplay,
 * the walking Moo on menus, shown on every VMU screen that is plugged in. See docs/vmu-lcd.md. */
#ifndef KLONOA_DC_HAL_VMU_LCD_H
#define KLONOA_DC_HAL_VMU_LCD_H

void hal_vmu_lcd_init(void);  /* once, at boot (main_dc.c) */
void hal_vmu_lcd_frame(void); /* once per game frame, after the VBlank handler (hal_frame.c) */

#endif
