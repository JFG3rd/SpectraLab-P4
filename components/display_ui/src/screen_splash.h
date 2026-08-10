#pragma once

/* Boot splash screen: product name + author credit, shown for a few seconds
 * while the rest of the system initializes, then replaced by the spectrum
 * screen (which must already be created). The splash object is deleted
 * automatically after the transition. */

/* How long the splash stays up, in seconds. 0 skips it entirely — the spectrum
 * screen is loaded straight away. Clamped to the range settings_sanitize()
 * enforces (0-15). Must be called before screen_splash_show(). */
void screen_splash_set_seconds(int seconds);

void screen_splash_show(void);
