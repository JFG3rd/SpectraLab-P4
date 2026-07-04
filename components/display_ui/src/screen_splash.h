#pragma once

/* Boot splash screen: application name + author credit, shown for a few
 * seconds while the rest of the system initializes, then fades into the
 * spectrum screen (which must already be created). The splash screen
 * object is deleted automatically after the transition. */

void screen_splash_show(void);
