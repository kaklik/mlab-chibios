#include <string.h>

#include "ch.h"
#include "hal.h"
#include "shell.h"
#include "evtimer.h"
#include "chprintf.h"

#include "ff.h"

/*===========================================================================*/
/* MMC/SPI related.                                                          */
/*===========================================================================*/

/**
 * @brief FS object.
 */
FATFS MMC_FS;
FRESULT err;

int connected = 0;
int tried = 0;

/**
 * MMC driver instance.
 */
MMCDriver MMCD1;

/* FS mounted and ready.*/
static bool_t fs_ready = FALSE;

/* Maximum speed SPI configuration (18MHz, CPHA=0, CPOL=0, MSb first).*/
static SPIConfig hs_spicfg = {NULL, IOPORT2, GPIOB_SPI2NSS, 0};

/* Low speed SPI configuration (281.250kHz, CPHA=0, CPOL=0, MSb first).*/
static SPIConfig ls_spicfg = {NULL, IOPORT2, GPIOB_SPI2NSS,
	SPI_CR1_BR_2 | SPI_CR1_BR_1};

/* Card insertion verification.*/
static bool_t mmc_is_inserted(void) {return !palReadPad(GPIOC, GPIOC_MMCCP);}

/* Card protection verification.*/
static bool_t mmc_is_protected(void) {return palReadPad(GPIOC, GPIOC_MMCWP);}

/* Generic large buffer.*/
uint8_t fbuff[1024];

static FRESULT scan_files(BaseChannel *chp, char *path) {
	FRESULT res;
	FILINFO fno;
	DIR dir;
	int i;
	char *fn;
	
#if _USE_LFN
	fno.lfname = 0;
	fno.lfsize = 0;
#endif
	res = f_opendir(&dir, path);
	if (res == FR_OK) {
		i = strlen(path);
		for (;;) {
			res = f_readdir(&dir, &fno);
			if (res != FR_OK || fno.fname[0] == 0)
				break;
			if (fno.fname[0] == '.')
				continue;
			fn = fno.fname;
			if (fno.fattrib & AM_DIR) {
				path[i++] = '/';
				strcpy(&path[i], fn);
				res = scan_files(chp, path);
				if (res != FR_OK)
					break;
				path[--i] = 0;
			}
			else {
				chprintf(chp, "%s/%s\r\n", path, fn);
			}
		}
	}
	return res;
}

/*===========================================================================*/
/* Command line related.                                                     */
/*===========================================================================*/

#define SHELL_WA_SIZE   THD_WA_SIZE(2048)
#define TEST_WA_SIZE    THD_WA_SIZE(256)

static void cmd_mem(BaseChannel *chp, int argc, char *argv[]) {
	size_t n, size;
	
	(void)argv;
	if (argc > 0) {
		chprintf(chp, "Usage: mem\r\n");
		return;
	}
	n = chHeapStatus(NULL, &size);
	chprintf(chp, "core free memory : %u bytes\r\n", chCoreStatus());
	chprintf(chp, "heap fragments   : %u\r\n", n);
	chprintf(chp, "heap free total  : %u bytes\r\n", size);
}

static void cmd_threads(BaseChannel *chp, int argc, char *argv[]) {
	static const char *states[] = {THD_STATE_NAMES};
	Thread *tp;
	
	(void)argv;
	if (argc > 0) {
		chprintf(chp, "Usage: threads\r\n");
		return;
	}
	chprintf(chp, "    addr    stack prio refs     state time\r\n");
	tp = chRegFirstThread();
	do {
		chprintf(chp, "%.8lx %.8lx %4lu %4lu %9s %lu\r\n",
				 (uint32_t)tp, (uint32_t)tp->p_ctx.r13,
				 (uint32_t)tp->p_prio, (uint32_t)(tp->p_refs - 1),
				 states[tp->p_state], (uint32_t)tp->p_time);
		tp = chRegNextThread(tp);
	} while (tp != NULL);
}

static void cmd_tree(BaseChannel *chp, int argc, char *argv[]) {
	FRESULT err;
	uint32_t clusters;
	FATFS *fsp;
	
	(void)argv;
	if (argc > 0) {
		chprintf(chp, "Usage: tree\r\n");
		return;
	}
	if (!fs_ready) {
		chprintf(chp, "File System not mounted\r\n");
		return;
	}
	err = f_getfree("/", &clusters, &fsp);
	if (err != FR_OK) {
		chprintf(chp, "FS: f_getfree() failed\r\n");
		return;
	}
	chprintf(chp,
			 "FS: %lu free clusters, %lu sectors per cluster, %lu bytes free\r\n",
			 clusters, (uint32_t)MMC_FS.csize,
			 clusters * (uint32_t)MMC_FS.csize * (uint32_t)MMC_SECTOR_SIZE);
	fbuff[0] = 0;
	scan_files(chp, (char *)fbuff);
}


static void cmd_card(BaseChannel *chp, int argc, char *argv[]) {
	chprintf(chp, "DETECT %d, PROTECT %d\r\n",
			 palReadPad(GPIOC, GPIOC_MMCCP),
			 palReadPad(GPIOC, GPIOC_MMCWP));
	
	chprintf(chp, "tried %d, connected %d\r\n", tried, connected);
	chprintf(chp, "inserted %d, protected %d\r\n",
			 mmc_is_inserted(), mmc_is_protected());
	chprintf(chp, "error je %d\r\n", err);
}

static const ShellCommand commands[] = {
	{"mem", cmd_mem},
	{"threads", cmd_threads},
	{"tree", cmd_tree},
	{"card", cmd_card},
	{NULL, NULL}
};

static const ShellConfig shell_cfg1 = {
	(BaseChannel *)&SD2,
	commands
};

/*
 * Red LEDs blinker thread, times are in milliseconds.
 */
static WORKING_AREA(waThread1, 128);
static msg_t Thread1(void *arg) {
	
	(void)arg;
	chRegSetThreadName("blinker");
	while (TRUE) {
		palTogglePad(GPIOB, 7);
		if (fs_ready)
			chThdSleepMilliseconds(200);
		else
			chThdSleepMilliseconds(500);
	}
	return 0;
}

/*
 * MMC card insertion event.
 */
static void InsertHandler(eventid_t id) {
	
	(void)id;
	/*
	 * On insertion MMC initialization and FS mount.
	 */
	
	tried = 1;
	connected = 5;
	if (mmcConnect(&MMCD1)) {
		connected = 2;
		return;
	}
	connected = 6;
	err = f_mount(0, &MMC_FS);
	connected = 7;
	if (err != FR_OK) {
		connected = 3;
		mmcDisconnect(&MMCD1);
		return;
	}
	connected = 1;
	fs_ready = TRUE;
}

/*
 * MMC card removal event.
 */
static void RemoveHandler(eventid_t id) {
	
	(void)id;
	fs_ready = FALSE;
}

/*
 * Application entry point.
 */
int main(void) {
	static const evhandler_t evhndl[] = {
		InsertHandler,
		RemoveHandler
	};
	Thread *shelltp = NULL;
	struct EventListener el0, el1;
	
	/*
	 * System initializations.
	 * - HAL initialization, this also initializes the configured device drivers
	 *   and performs the board-specific initializations.
	 * - Kernel initialization, the main() function becomes a thread and the
	 *   RTOS is active.
	 */
	halInit();
	chSysInit();
	
	/*
	 * Activates the serial driver 2 using the driver default configuration.
	 */
	sdStart(&SD2, NULL);
	
	/*
	 * Shell manager initialization.
	 */
	shellInit();
	
	/*
	 * Initializes the MMC driver to work with SPI2.
	 */
	palSetPadMode(GPIOB, GPIOB_SPI2NSS, PAL_MODE_OUTPUT_PUSHPULL);
	palSetPad(GPIOB, GPIOB_SPI2NSS);
	mmcObjectInit(&MMCD1, &SPID2,
				  &ls_spicfg, &hs_spicfg,
				  mmc_is_protected, mmc_is_inserted);
	mmcStart(&MMCD1, NULL);
	
	/*
	 * Creates the blinker thread.
	 */
	chThdCreateStatic(waThread1, sizeof(waThread1), NORMALPRIO, Thread1, NULL);
	
	/*
	 * Normal main() thread activity, in this demo it does nothing except
	 * sleeping in a loop and listen for events.
	 */
	chEvtRegister(&MMCD1.inserted_event, &el0, 0);
	chEvtRegister(&MMCD1.removed_event, &el1, 1);
	while (TRUE) {
		if (!shelltp)
			shelltp = shellCreate(&shell_cfg1, SHELL_WA_SIZE, NORMALPRIO);
		else if (chThdTerminated(shelltp)) {
			chThdRelease(shelltp);    /* Recovers memory of the previous shell.   */
			shelltp = NULL;           /* Triggers spawning of a new shell.        */
		}
		chEvtDispatch(evhndl, chEvtWaitOne(ALL_EVENTS));
	}
	return 0;
}
