#ifndef PLAYER_X2600_SYSCLK_CONF_H
#define PLAYER_X2600_SYSCLK_CONF_H

/* HALLEY7-M normal NOR boot profile, not the USB helper's 800/400 MHz profile. */
#define SYSCLK_EXTAL 24000000UL
#define SYSCLK_APLL 1152000000UL
#define SYSCLK_MPLL 1800000000UL
#define SystemCoreClock SYSCLK_APLL

#endif
