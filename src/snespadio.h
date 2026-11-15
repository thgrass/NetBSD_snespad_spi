/*	$NetBSD$	*/

#ifndef _DEV_SPI_SNESPADIO_H_
#define _DEV_SPI_SNESPADIO_H_

#include <sys/types.h>

/*
 * Button bit numbers used internally for the wskbd(4) key mapping
 * (1 << bit = pressed).  The SNES pad reports its 12 buttons active-low,
 * MSB first, in the order B, Y, Select, Start, Up, Down, Left, Right,
 * A, X, L, R.
 */
#define SNESPAD_BTN_R 0x0001
#define SNESPAD_BTN_L 0x0002
#define SNESPAD_BTN_X 0x0004
#define SNESPAD_BTN_A 0x0008
#define SNESPAD_BTN_RIGHT 0x0010
#define SNESPAD_BTN_LEFT 0x0020
#define SNESPAD_BTN_DOWN 0x0040
#define SNESPAD_BTN_UP 0x0080
#define SNESPAD_BTN_START 0x0100
#define SNESPAD_BTN_SELECT 0x0200
#define SNESPAD_BTN_Y 0x0400
#define SNESPAD_BTN_B 0x0800
#define SNESPAD_NBUTTONS 12

#endif /* _DEV_SPI_SNESPADIO_H_ */
