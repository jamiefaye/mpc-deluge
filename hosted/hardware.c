// The Deluge's hardware, absent. Every function here replaces a driver the firmware calls;
// none of them does anything except where a comment says otherwise. Prototypes are written out
// rather than taken from the firmware's driver headers, which are full of register definitions;
// these are all C symbols, so only the names and the calling shape have to agree.
//
// SPDX-License-Identifier: GPL-3.0-or-later
#include "hosted.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// --- debug text ----------------------------------------------------------------------------------
void deluge_hosted_log(const char* text) {
	static int enabled = -1;
	if (enabled < 0) {
		// Also on when a file called deluge-log exists in the working directory, for places
		// where setting an environment variable is awkward (the emulated test machine).
		enabled = getenv("DELUGE_LOG") != NULL || access("deluge-log", F_OK) == 0;
	}
	if (enabled) {
		fputs(text, stderr);
	}
}
void uartPrint(char const* output) {
	deluge_hosted_log(output);
}
void uartPrintln(char const* output) {
	deluge_hosted_log(output);
	deluge_hosted_log("\n");
}
void uartPrintNumber(int32_t number) {
	char text[16];
	snprintf(text, sizeof text, "%d\n", (int)number);
	deluge_hosted_log(text);
}
void uartPrintlnFloat(float number) {
	char text[32];
	snprintf(text, sizeof text, "%f\n", (double)number);
	deluge_hosted_log(text);
}

// --- UARTs (PIC and MIDI) ------------------------------------------------------------------------
// Zeroed storage, generously sized: the firmware indexes these but nothing is ever sent.
char uartItems[4096] __attribute__((aligned(16)));
char picTxBuffer[65536];
char midiTxBuffer[65536];
void uartFlushIfNotSending(int32_t item) {
	(void)item;
}
uint8_t uartGetChar(int32_t item, char* readData) {
	(void)item;
	(void)readData;
	return 0; // nothing received
}
uint32_t* uartGetCharWithTiming(int32_t timingCaptureItem, char* readData) {
	(void)timingCaptureItem;
	(void)readData;
	return NULL; // nothing received
}
int32_t uartGetTxBufferFullnessByItem(int32_t item) {
	(void)item;
	return 0;
}
int32_t uartGetTxBufferSpace(int32_t item) {
	(void)item;
	return 1024;
}
void uartPutCharBack(int32_t item) {
	(void)item;
}

// --- audio codec (SSI) buffers -------------------------------------------------------------------
// The firmware's audio routine reads where the DMA is in these to decide how much to render. The
// plugin calls the render functions directly instead; these exist so the pointers are valid.
static int32_t ssiTxBuffer[8192];
static int32_t ssiRxBuffer[8192];
int32_t* getTxBufferStart(void) {
	return ssiTxBuffer;
}
int32_t* getTxBufferEnd(void) {
	return ssiTxBuffer + 8192;
}
void* getTxBufferCurrentPlace(void) {
	return ssiTxBuffer;
}
int32_t* getRxBufferStart(void) {
	return ssiRxBuffer;
}
int32_t* getRxBufferEnd(void) {
	return ssiRxBuffer + 8192;
}
void* getRxBufferCurrentPlace(void) {
	return ssiRxBuffer;
}

// --- timers --------------------------------------------------------------------------------------
// TIMER_SYSTEM_FAST and friends are 16-bit counters at fixed rates. One sample at 44.1 kHz is
// 755.8 ticks of the 33.33 MHz system clock; the firmware divides that down per timer, but
// nothing in the audio path depends on the exact rate, so every timer runs at the undivided rate.
static uint32_t timerTicks;
static uint32_t timerOffset[5];
void deluge_hosted_advance_samples(uint32_t numSamples) {
	timerTicks += numSamples * 756u;
}
uint32_t getTimerValue(int timerNo) {
	return (timerTicks - timerOffset[timerNo % 5]) & 0xFFFF;
}
void setTimerValue(int timerNo, uint32_t timerValue) {
	timerOffset[timerNo % 5] = timerTicks - timerValue;
}
void enableTimer(int timerNo) {
	(void)timerNo;
}
void disableTimer(int timerNo) {
	(void)timerNo;
}
void setOperatingMode(int timerNo, int mode, bool enable_interrupt) {
	(void)timerNo;
	(void)mode;
	(void)enable_interrupt;
}

// --- display, pads, CV, flash, GPIO, interrupts, cache --------------------------------------------
int oledWaitingForMessage = 256;
volatile bool spiTransferQueueCurrentlySending = false;
volatile uint32_t intc_func_active = 0;
uint8_t currentlyAccessingCard = 0;
void oledRoutine(void) {}
void oledSelectingComplete(void) {}
void oledDeselectionComplete(void) {}
void oledLowLevelTimerCallback(void) {}
void enqueueSPITransfer(int32_t whichOled, uint8_t const* image) {
	(void)whichOled;
	(void)image;
}
void enqueueCVMessage(int channel, uint32_t message) {
	(void)channel;
	(void)message;
}
void R_RSPI_SendBasic32(uint8_t channel, uint32_t data) {
	(void)channel;
	(void)data;
}
int32_t R_INTC_Enable(uint16_t int_id) {
	(void)int_id;
	return 0;
}
// Settings would be written to the Deluge's flash chip. Not kept.
int32_t R_SFLASH_EraseSector(uint32_t addr, uint32_t ch_no, uint32_t dual, uint8_t data_width, uint8_t addr_mode) {
	(void)addr;
	(void)ch_no;
	(void)dual;
	(void)data_width;
	(void)addr_mode;
	return 0;
}
int32_t R_SFLASH_ByteProgram(uint32_t addr, uint8_t* buf, int32_t size, uint32_t ch_no, uint32_t dual, uint8_t data_width,
                             uint8_t addr_mode) {
	(void)addr;
	(void)buf;
	(void)size;
	(void)ch_no;
	(void)dual;
	(void)data_width;
	(void)addr_mode;
	return 0;
}
uint16_t readInput(uint8_t p, uint8_t q) {
	(void)p;
	(void)q;
	return 0;
}
void setOutputState(uint8_t p, uint8_t q, uint16_t state) {
	(void)p;
	(void)q;
	(void)state;
}
void setPinAsInput(uint8_t p, uint8_t q) {
	(void)p;
	(void)q;
}
void setPinAsOutput(uint8_t p, uint8_t q) {
	(void)p;
	(void)q;
}
void setPinMux(uint8_t p, uint8_t q, uint8_t mux) {
	(void)p;
	(void)q;
	(void)mux;
}
void v7_dma_flush_range(uintptr_t start, uintptr_t end) {
	(void)start;
	(void)end;
}
void fault_handler_print_freeze_pointers(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
	(void)a;
	(void)b;
	(void)c;
	(void)d;
}

// --- USB -----------------------------------------------------------------------------------------
void* g_p_usb_pipe[64];
uint16_t g_usb_hmidi_tmp_ep_tbl[4096];
uint16_t g_usb_peri_connected = 0;
uint16_t g_usb_usbmode = 0;
static char usbRegisters[65536];
void* usb_hstd_get_usb_ip_adr(uint16_t ipno) {
	(void)ipno;
	return usbRegisters;
}
void change_destination_of_send_pipe(void) {}
void usb_send_start_rohan(void) {}
