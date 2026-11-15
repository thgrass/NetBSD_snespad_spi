/*	$NetBSD$	*/

/*
 * "SNES-Gamepads-to-SPI-Adaptor" board
 * (https://github.com/thgrass/SNES-Gamepads-to-SPI-Adaptor).
 *
 * Hardware: the adaptor (made for the Raspberry Pi SPI0 header pins
 * SCLK=GPIO11, MISO=GPIO9, CE0=GPIO8, CE1=GPIO7, +5V, GND, but on other
 * machines just wire SCLK/MISO/CE0/CE1/+5V/GND to any spi(4) port) carries
 * two SNES controller sockets. Each pad's Latch pin is wired directly
 * to one of the chip selects (CE0 for pad 1, CE1 for pad 2, ...), and
 * each pad's Data line reaches the shared MISO line through one gate of
 * a 74AHCT125 whose output-enable is that same chip select. So while a
 * CE is asserted the pad is in serial-shift mode and drives MISO; while
 * the CE is released (idle high) the pad continuously parallel-loads the
 * button states (its latch is active high).
 *
 * Electrically the pad is therefore a plain SPI mode-0 slave that only
 * has MISO: asserting CE (latch low) starts a transfer, and 16 clocks
 * shift out the button states MSB first, active low, in the order
 * B, Y, Select, Start, Up, Down, Left, Right, A, X, L, R, followed by
 * four ones (the pad's unused inputs are pulled up).  With nothing
 * pressed the raw word is 0xffff.
 *
 * The driver attaches only through the generic spi(4) bus
 * ("snespad* at spi? slave N") and works on any NetBSD machine with an
 * SPI port. The slave number is the chip-select index on that bus.
 *
 * Each pad is exposed as a wskbd(4) keyboard, so the pads work as plain
 * keyboards for wscons/X11 applications and emulators. Up to
 * SNESPAD_MAX_PORTS (4) pads are supported ("snespad* at spi? slave N");
 * each port maps its buttons to a different key set (see
 * snespad_keycode[][]), so multiple pads on the same keyboard remain
 * distinguishable by key. Button changes can be logged via the
 * hw.snespad_debug sysctl.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/device.h>
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/kthread.h>
#include <sys/mutex.h>
#include <sys/condvar.h>
#include <sys/bus.h>
#include <sys/module.h>
#include <sys/sysctl.h>

#include <dev/spi/spivar.h>
#include <dev/spi/snespadio.h>

#include <dev/wscons/wsconsio.h>
#include <dev/wscons/wskbdvar.h>
#include <dev/wscons/wsksymdef.h>
#include <dev/wscons/wsksymvar.h>

#include "ioconf.h"
#include "locators.h"

#ifndef SPICF_SLAVE
#define	SPICF_SLAVE	0	/* dev/spi/files.spi: device spi { [slave = -1] } */
#endif

/* SPI mode 0 (CPOL=0, CPHA=0), MSB first, and speed for the spi(4) attach */
#define	SNESPAD_SPI_SPEED	500000		/* Hz */

/* poll the pads at ~60 Hz */
static int
snespad_poll_ticks(void)
{
	int t = hz / 60;
	return t < 1 ? 1 : t;
}

/*
 * Maximum number of pads (chip selects) supported.  Default 4; override
 * at compile time, e.g. CPPFLAGS+= -DSNESPAD_MAX_PORTS=2 in the module
 * Makefile, to match buses with fewer chip selects.  Pads beyond the
 * bus's number of CS lines never attach (spi_search() rejects them).
 */
#ifndef SNESPAD_MAX_PORTS
#define	SNESPAD_MAX_PORTS	4
#endif

/*
 * USB HID keycodes the buttons are mapped to for the wskbd(4) side.
 * Indexed by [slave][SNESPAD_BTN_* bit number]; each port gets its own
 * key set so that up to four pads on the same keyboard are still
 * distinguishable by the key they produce:
 *   port 0: arrows + a/b/x/y/l/r + Return/Space
 *   port 1: numeric keypad
 *   port 2: F1..F12
 *   port 3: c,d,e,f,g,h,i,j,k,m,n,o
 */
static const uint8_t snespad_keycode[SNESPAD_MAX_PORTS][SNESPAD_NBUTTONS] = {
    /* port 0 */
    { 0x0f,	/* R      -> 'r'		*/
      0x0e,	/* L      -> 'l'		*/
      0x1b,	/* X      -> 'x'		*/
      0x04,	/* A      -> 'a'		*/
      0x4f,	/* Right  -> arrow right	*/
      0x50,	/* Left   -> arrow left	*/
      0x51,	/* Down   -> arrow down	*/
      0x52,	/* Up     -> arrow up	*/
      0x28,	/* Start  -> return	*/
      0x2c,	/* Select -> space	*/
      0x1c,	/* Y      -> 'y'		*/
      0x05	/* B      -> 'b'		*/ },
    /* port 1: numeric keypad */
    { 0x57,	/* R      -> KP +	*/
      0x56,	/* L      -> KP -	*/
      0x61,	/* X      -> KP 9	*/
      0x59,	/* A      -> KP 1	*/
      0x5e,	/* Right  -> KP 6	*/
      0x5c,	/* Left   -> KP 4	*/
      0x5a,	/* Down   -> KP 2	*/
      0x60,	/* Up     -> KP 8	*/
      0x58,	/* Start  -> KP Enter	*/
      0x62,	/* Select -> KP 0	*/
      0x5f,	/* Y      -> KP 7	*/
      0x5b	/* B      -> KP 3	*/ },
    /* port 2: F1..F12 */
    { 0x45,	/* R      -> F12	*/
      0x44,	/* L      -> F11	*/
      0x43,	/* X      -> F10	*/
      0x42,	/* A      -> F9		*/
      0x41,	/* Right  -> F8		*/
      0x40,	/* Left   -> F7		*/
      0x3f,	/* Down   -> F6		*/
      0x3e,	/* Up     -> F5		*/
      0x3d,	/* Start  -> F4		*/
      0x3c,	/* Select -> F3		*/
      0x3b,	/* Y      -> F2		*/
      0x3a	/* B      -> F1		*/ },
    /* port 3: c d e f g h i j k m n o */
    { 0x12,	/* R      -> 'o'		*/
      0x11,	/* L      -> 'n'		*/
      0x08,	/* X      -> 'e'		*/
      0x06,	/* A      -> 'c'		*/
      0x10,	/* Right  -> 'm'	*/
      0x0e,	/* Left   -> 'k'		*/
      0x0d,	/* Down   -> 'j'		*/
      0x0c,	/* Up     -> 'i'		*/
      0x0a,	/* Start  -> 'g'		*/
      0x0b,	/* Select -> 'h'		*/
      0x09,	/* Y      -> 'f'		*/
      0x07	/* B      -> 'd'		*/ },
};

/* names for snespad_debug output, indexed by SNESPAD_BTN_* bit number */
static const char * const snespad_btnname[SNESPAD_NBUTTONS] = {
	"R", "L", "X", "A", "Right", "Left", "Down", "Up",
	"Start", "Select", "Y", "B"
};

/*
 * hw.snespad_debug: 0 = off, 1 = log button changes, 2 = log every sample.
 * Registered by the module's modcmd; visible via sysctl(8) and dmesg(8).
 */
static int snespad_debug = 0;
static struct sysctllog *snespad_clog;

struct snespad_softc {
	device_t	sc_dev;
	int			sc_slave;	    /* chip select (slave) number */
	int			sc_ready;
	struct spi_handle	*sc_sh;	/* spi(4) handle */

	kmutex_t		sc_lock;
	kcondvar_t		sc_cv;		/* thread wakeup */

	uint16_t		sc_buttons;	/* pressed button mask */
	uint16_t		sc_raw;		/* last raw word from the pad */
	uint32_t		sc_seq;		/* state changes since attach */
	uint64_t		sc_errors;	/* failed SPI transfers */

	device_t		sc_wskbddev;
	int			sc_enabled;

	lwp_t			*sc_lwp;
	bool			sc_dying;
};

static int	snespad_match(device_t, cfdata_t, void *);
static void	snespad_attach(device_t, device_t, void *);
static void	snespad_doattach(device_t);
static int	snespad_detach(device_t, int);

static void	snespad_thread(void *);
static int	snespad_read_raw(struct snespad_softc *, uint16_t *);
static void	snespad_sample(struct snespad_softc *);
static uint16_t	snespad_pressed(uint16_t);

static int	snespad_wskbd_enable(void *, int);
static void	snespad_wskbd_set_leds(void *, int);
static int	snespad_wskbd_ioctl(void *, u_long, void *, int, lwp_t *);

CFATTACH_DECL_NEW(snespad, sizeof(struct snespad_softc),
    snespad_match, snespad_attach, snespad_detach, NULL);
/* named attachments, in case files.spi uses "with snespad_root/_spi" */
CFATTACH_DECL_NEW(snespad_root, sizeof(struct snespad_softc),
    snespad_match, snespad_attach, snespad_detach, NULL);
CFATTACH_DECL_NEW(snespad_spi, sizeof(struct snespad_softc),
    snespad_match, snespad_attach, snespad_detach, NULL);

extern const struct wscons_keydesc hidkbd_keydesctab[];
static const struct wskbd_mapdata snespad_keymapdata = {
	hidkbd_keydesctab,
	KB_US,
};

static const struct wskbd_accessops snespad_accessops = {
	.enable = snespad_wskbd_enable,
	.set_leds = snespad_wskbd_set_leds,
	.ioctl = snespad_wskbd_ioctl
};

/*
 * The wskbd(4) child is wired up with the usual kernel config line
 * "wskbd* at wskbddev? mux 1" (snespad provides the wskbddev attribute);
 * config_found() below matches that cfdata, at boot and at runtime.
 */

static int
snespad_match(device_t parent, cfdata_t cf, void *aux)
{

	return 1;
}

static void
snespad_attach(device_t parent, device_t self, void *aux)
{
	struct snespad_softc * const sc = device_private(self);

	sc->sc_dev = self;

	/* generic attach: snespad* at spi? slave N */
	struct spi_attach_args * const sa = aux;
	cfdata_t cf = device_cfdata(self);

	if (sa == NULL) {
		aprint_error_dev(self, "must attach to an spi(4) bus\n");
		return;
	}
	sc->sc_sh = sa->sa_handle;
	sc->sc_slave = cf->cf_loc[SPICF_SLAVE];
	if (sc->sc_slave < 0 || sc->sc_slave >= SNESPAD_MAX_PORTS) {
		aprint_error_dev(self, "bad slave %d\n", sc->sc_slave);
		return;
	}

	mutex_init(&sc->sc_lock, MUTEX_DEFAULT, IPL_NONE);
	cv_init(&sc->sc_cv, "snespad");
	sc->sc_enabled = 1;

	aprint_naive("\n");
	aprint_normal(": SNES gamepad, SPI slave %d\n", sc->sc_slave);

	config_interrupts(self, snespad_doattach);
}

static void
snespad_doattach(device_t self)
{
	struct snespad_softc * const sc = device_private(self);
	struct wskbddev_attach_args a;
	uint16_t raw;
	int error;

	/* configure the spi(4) handle: mode 0 (CPOL=0, CPHA=0), MSB first */
	{
		error = spi_configure(self, sc->sc_sh, SPI_MODE_0,
		    SNESPAD_SPI_SPEED);
		if (error != 0) {
			aprint_error_dev(self, "unable to configure SPI (%d)\n",
			    error);
			return;
		}
		sc->sc_ready = 1;
	}
	/* probe: an idle pad reads 0xffff (all button lines high) */
	if (snespad_read_raw(sc, &raw) != 0) {
		aprint_error_dev(self, "SPI transfer failed\n");
		return;
	}
	if ((raw & 0x000f) != 0x000f)
		aprint_normal_dev(self, "no gamepad detected (raw 0x%04x)\n", raw);
	else if (raw == 0xffff)
		aprint_normal_dev(self, "gamepad detected (idle)\n");
	else
		aprint_normal_dev(self, "gamepad detected (raw 0x%04x)\n", raw);

	mutex_enter(&sc->sc_lock);
	sc->sc_raw = raw;
	sc->sc_buttons = snespad_pressed(raw);
	mutex_exit(&sc->sc_lock);

	memset(&a, 0, sizeof(a));
	a.console = 0;
	a.keymap = &snespad_keymapdata;
	a.accessops = &snespad_accessops;
	a.accesscookie = sc;
	sc->sc_wskbddev = config_found(self, &a, wskbddevprint, CFARGS_NONE);

	if (kthread_create(PRI_NONE, 0, NULL, snespad_thread, sc,
	    &sc->sc_lwp, "snespad%d", device_unit(self)) != 0) {
		aprint_error_dev(self, "unable to create kernel thread\n");
		sc->sc_lwp = NULL;
	}
}

static int
snespad_detach(device_t self, int flags)
{
	struct snespad_softc * const sc = device_private(self);

	mutex_enter(&sc->sc_lock);
	sc->sc_dying = true;
	cv_broadcast(&sc->sc_cv);
	while (sc->sc_lwp != NULL)
		cv_wait(&sc->sc_cv, &sc->sc_lock);
	mutex_exit(&sc->sc_lock);

	cv_destroy(&sc->sc_cv);
	mutex_destroy(&sc->sc_lock);

	return config_detach_children(self, flags);
}

/*
 * Read the pad's 16-bit word: clock out two dummy bytes and sample MISO.
 */
static int
snespad_read_raw(struct snespad_softc * const sc, uint16_t *rawp)
{
	uint8_t buf[2];

	if (spi_recv(sc->sc_sh, sizeof(buf), buf) != 0)
		return EIO;

	*rawp = (uint16_t)((buf[0] << 8) | buf[1]);
	return 0;
}


/*
 * A connected pad always shifts out four 1 bits after the 12 button bits,
 * so (raw & 0xf) == 0xf means a pad is present.  An empty socket has a
 * floating data line (often read as 0x0000, i.e. "all buttons pressed"):
 * report no buttons unless the trailing ones are there, so a missing pad
 * cannot generate phantom key events.
 */
static uint16_t
snespad_pressed(uint16_t raw)
{

	if ((raw & 0x000f) != 0x000f)
		return 0;
	return (uint16_t)((~raw >> 4) & 0x0fff);
}

static void
snespad_thread(void *arg)
{
	struct snespad_softc * const sc = arg;
	const int ticks = snespad_poll_ticks();

	mutex_enter(&sc->sc_lock);
	while (!sc->sc_dying) {
		mutex_exit(&sc->sc_lock);
		snespad_sample(sc);
		mutex_enter(&sc->sc_lock);
		if (!sc->sc_dying)
			(void)cv_timedwait(&sc->sc_cv, &sc->sc_lock, ticks);
	}
	sc->sc_lwp = NULL;
	cv_broadcast(&sc->sc_cv);
	mutex_exit(&sc->sc_lock);
	kthread_exit(0);
}

static void
snespad_debug_event(struct snespad_softc * const sc, uint16_t raw,
    uint16_t pressed, uint16_t changed)
{
	char buf[160];
	size_t off;
	int i;

	if (snespad_debug >= 2)
		aprint_normal_dev(sc->sc_dev, "sample raw=0x%04x buttons=0x%03x\n",
		    raw, pressed);
	if (snespad_debug < 1 || changed == 0)
		return;

	off = (size_t)snprintf(buf, sizeof(buf),
	    "raw=0x%04x buttons=0x%03x seq=%u:", raw, pressed, sc->sc_seq);
	for (i = 0; i < SNESPAD_NBUTTONS && off < sizeof(buf) - 10; i++) {
		if ((changed & (1u << i)) == 0)
			continue;
		off += (size_t)snprintf(buf + off, sizeof(buf) - off, " %s%s",
		    (pressed & (1u << i)) != 0 ? "+" : "-", snespad_btnname[i]);
	}
	aprint_normal_dev(sc->sc_dev, "%s\n", buf);
}

static void
snespad_sample(struct snespad_softc * const sc)
{
	uint16_t raw, pressed, changed;
	int error, i;

	if (!sc->sc_ready)
		return;

	error = snespad_read_raw(sc, &raw);
	if (error != 0) {
		mutex_enter(&sc->sc_lock);
		sc->sc_errors++;
		mutex_exit(&sc->sc_lock);
		return;
	}

	pressed = snespad_pressed(raw);

	mutex_enter(&sc->sc_lock);
	changed = pressed ^ sc->sc_buttons;
	snespad_debug_event(sc, raw, pressed, changed);
	if (changed != 0) {
		if (sc->sc_wskbddev != NULL && sc->sc_enabled != 0) {
			for (i = 0; i < SNESPAD_NBUTTONS; i++) {
				if ((changed & (1u << i)) == 0)
					continue;
				wskbd_input(sc->sc_wskbddev,
				    (pressed & (1u << i)) != 0 ?
					WSCONS_EVENT_KEY_DOWN :
					WSCONS_EVENT_KEY_UP,
				    snespad_keycode[sc->sc_slave][i]);
			}
		}
		sc->sc_buttons = pressed;
		sc->sc_raw = raw;
		sc->sc_seq++;
		cv_broadcast(&sc->sc_cv);
	}
	mutex_exit(&sc->sc_lock);
}

static int
snespad_wskbd_enable(void *v, int on)
{
	struct snespad_softc * const sc = v;

	sc->sc_enabled = on;

	return 0;
}

static void
snespad_wskbd_set_leds(void *v, int leds)
{
}

static int
snespad_wskbd_ioctl(void *v, u_long cmd, void *data, int flag, lwp_t *l)
{

	switch (cmd) {
	case WSKBDIO_GTYPE:
		*(int *)data = WSKBD_TYPE_USB;
		return 0;
	default:
		return EPASSTHROUGH;
	}
}

MODULE(MODULE_CLASS_DRIVER, snespad, NULL);

#ifdef _MODULE
#include "ioconf.c"
#endif

static int
snespad_modcmd(modcmd_t cmd, void *opaque)
{
#ifdef _MODULE
	int error;
#endif

	switch (cmd) {
	case MODULE_CMD_INIT:
		/*
		 * The sysctl is set up for built-in builds too (module
		 * init runs after sysctl_init()).  The cfdata is only
		 * needed for LKMs: a built-in driver gets its cfdata
		 * from the kernel's own ioconf, and autoconf
		 * attaches the configured instances at boot by itself.
		 */
		sysctl_createv(&snespad_clog, 0, NULL, NULL,
		    CTLFLAG_PERMANENT | CTLFLAG_READWRITE,
		    CTLTYPE_INT, "snespad_debug",
		    SYSCTL_DESCR("log snespad button events to dmesg: "
			"0=off, 1=changes, 2=every sample"),
		    NULL, 0, &snespad_debug, sizeof(snespad_debug),
		    CTL_HW, CTL_CREATE, CTL_EOL);
#ifdef _MODULE
		error = config_init_component(cfdriver_ioconf_snespad,
		    cfattach_ioconf_snespad, cfdata_ioconf_snespad);
		if (error != 0)
			return error;
		/*
		 * "snespad* at spi? slave N" instances are attached by
		 * the rescan that config_init_component() triggers on
		 * every bus implementing ca_rescan (needs the spi_rescan
		 * patch on stock spi(4); otherwise build the driver into
         * the kernel).
		 */
#endif
		return 0;
	case MODULE_CMD_FINI:
#ifdef _MODULE
		error = config_fini_component(cfdriver_ioconf_snespad,
		    cfattach_ioconf_snespad, cfdata_ioconf_snespad);
		if (error != 0)
			return error;
#endif
		sysctl_teardown(&snespad_clog);
		return 0;
	default:
		return ENOTTY;
	}
}
