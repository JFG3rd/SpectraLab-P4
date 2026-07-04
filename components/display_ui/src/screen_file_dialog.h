#pragma once

/* SD preset file dialogs:
 *  - Save-As screen: on-screen keyboard to name a preset file
 *  - File browser screen: list presets with Load / Rename / Delete
 * Both are full LVGL screens created lazily on first show and return to
 * the settings screen when done. */

/* Open the Save-As dialog (saves the current settings UI state). */
void screen_saveas_show(void);

/* Open the preset file browser (Load / Rename / Delete). */
void screen_files_show(void);
