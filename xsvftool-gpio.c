/*
 *  Lib(X)SVF  -  A library for implementing SVF and XSVF JTAG players
 *
 *  Copyright (C) 2009  RIEGL Research ForschungsGmbH
 *  Copyright (C) 2009  Clifford Wolf <clifford@clifford.at>
 *  
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *  
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include "libxsvf.h"

#include <sys/time.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>

#define BCM2708_PERI_BASE        0x3F000000
#define GPIO_BASE                (BCM2708_PERI_BASE + 0x200000) /* GPIO   */

#define PAGE_SIZE (4*1024)
#define BLOCK_SIZE (4*1024)

// GPIO setup macros. Always use INP_GPIO(x) before using OUT_GPIO(x) or SET_GPIO_ALT(x,y)
#define INP_GPIO(g) *(gpio+((g)/10)) &= ~(7<<(((g)%10)*3))
#define OUT_GPIO(g) *(gpio+((g)/10)) |=  (1<<(((g)%10)*3))
#define SET_GPIO_ALT(g,a) *(gpio+(((g)/10))) |= (((a)<=3?(a)+4:(a)==4?3:2)<<(((g)%10)*3))

#define GPIO_SET0   *(gpio+7)  // Set GPIO high bits 0-31
#define GPIO_SET1   *(gpio+8)  // Set GPIO high bits 32-53

#define GPIO_CLR0   *(gpio+10) // Set GPIO low bits 0-31
#define GPIO_CLR1   *(gpio+11) // Set GPIO low bits 32-53

#define GPIO_IN0   *(gpio+13)  // Reads GPIO input bits 0-31

#define TDI_PIN 24
#define TMS_PIN 25
#define TCK_PIN 22
#define TDO_PIN 23

static volatile struct io_layout *io_direction;
static volatile struct io_layout *io_opendrain;
static volatile struct io_layout *io_data;

int  mem_fd;
char *gpio_mem, *gpio_map;
volatile unsigned *gpio;

static void io_setup(void)
{

	if (( mem_fd = open("/dev/mem", O_RDWR|O_SYNC) ) < 0) {
		printf("Can't open /dev/mem\n");
		printf("Did you forgot to use 'sudo .. ?'\n");
		exit (-1);
	}

	/*
	* mmap GPIO
	*/
	if ((gpio_mem = malloc(BLOCK_SIZE + (PAGE_SIZE-1))) == NULL) {
		printf("allocation error \n");
		exit (-1);
	}
	if ((unsigned long)gpio_mem % PAGE_SIZE)
		gpio_mem += PAGE_SIZE - ((unsigned long)gpio_mem % PAGE_SIZE);

		gpio_map = (unsigned char *)mmap(
		(caddr_t)gpio_mem,
		BLOCK_SIZE,
		PROT_READ|PROT_WRITE,
		MAP_SHARED|MAP_FIXED,
		mem_fd,
		GPIO_BASE
	);

	if ((long)gpio_map < 0) {
		printf("gpio mmap error %d\n", (int)gpio_map);
		exit (-1);
	}
	gpio = (volatile unsigned *)gpio_map;

	INP_GPIO(TDI_PIN);  OUT_GPIO(TDI_PIN);
	INP_GPIO(TMS_PIN);  OUT_GPIO(TMS_PIN);
	INP_GPIO(TCK_PIN);  OUT_GPIO(TCK_PIN);
	INP_GPIO(TDO_PIN);

}

static void io_shutdown(void)
{
	munmap(gpio_map,BLOCK_SIZE);
}

static void io_tms(int val)
{
	if(val == 0)GPIO_CLR0 = (1<<TMS_PIN);
	else GPIO_SET0 = (1<<TMS_PIN);
}

static void io_tdi(int val)
{
	if(val == 0)GPIO_CLR0 = (1<<TDI_PIN);
	else GPIO_SET0 = (1<<TDI_PIN);
}

static void io_tck(int val)
{
	if(val == 0)GPIO_CLR0 = (1<<TCK_PIN);
	else GPIO_SET0 = (1<<TCK_PIN);
}

static void io_sck(int val)
{
	/* not available */
}

static void io_trst(int val)
{
	/* not available */
}

static int io_tdo()
{
	unsigned int din;
	din = GPIO_IN0;
	din = (din >> TDO_PIN ) & 0x01;	// We just want bit TDO_PIN
	return( (unsigned char) din );
}


/** END: Low-Level I/O Implementation **/


struct udata_s {
	FILE *f;
	int verbose;
	int clockcount;
	int bitcount_tdi;
	int bitcount_tdo;
	int retval_i;
	int retval[256];
};

static int h_setup(struct libxsvf_host *h)
{
	struct udata_s *u = h->user_data;
	if (u->verbose >= 2) {
		fprintf(stderr, "[SETUP]\n");
		fflush(stderr);
	}
	io_setup();
	return 0;
}

static int h_shutdown(struct libxsvf_host *h)
{
	struct udata_s *u = h->user_data;
	if (u->verbose >= 2) {
		fprintf(stderr, "[SHUTDOWN]\n");
		fflush(stderr);
	}
	io_shutdown();
	return 0;
}

static void h_udelay(struct libxsvf_host *h, long usecs, int tms, long num_tck)
{
	struct udata_s *u = h->user_data;
	if (u->verbose >= 3) {
		fprintf(stderr, "[DELAY:%ld, TMS:%d, NUM_TCK:%ld]\n", usecs, tms, num_tck);
		fflush(stderr);
	}
	if (num_tck > 0) {
		struct timeval tv1, tv2;
		gettimeofday(&tv1, NULL);
		io_tms(tms);
		while (num_tck > 0) {
			io_tck(0);
			io_tck(1);
			num_tck--;
		}
		gettimeofday(&tv2, NULL);
		if (tv2.tv_sec > tv1.tv_sec) {
			usecs -= (1000000 - tv1.tv_usec) + (tv2.tv_sec - tv1.tv_sec - 1) * 1000000;
			tv1.tv_usec = 0;
		}
		usecs -= tv2.tv_usec - tv1.tv_usec;
		if (u->verbose >= 3) {
			fprintf(stderr, "[DELAY_AFTER_TCK:%ld]\n", usecs > 0 ? usecs : 0);
			fflush(stderr);
		}
	}
	if (usecs > 0) {
		usleep(usecs);
	}
}

static int h_getbyte(struct libxsvf_host *h)
{
	struct udata_s *u = h->user_data;
	return fgetc(u->f);
}

static int h_pulse_tck(struct libxsvf_host *h, int tms, int tdi, int tdo, int rmask, int sync)
{
	struct udata_s *u = h->user_data;

	io_tms(tms);

	if (tdi >= 0) {
		u->bitcount_tdi++;
		io_tdi(tdi);
	}

	io_tck(0);
	io_tck(1);

	int line_tdo = io_tdo();
	int rc = line_tdo >= 0 ? line_tdo : 0;

	if (rmask == 1 && u->retval_i < 256)
		u->retval[u->retval_i++] = line_tdo;

	if (tdo >= 0 && line_tdo >= 0) {
		u->bitcount_tdo++;
		if (tdo != line_tdo)
			rc = -1;
	}

	if (u->verbose >= 4) {
		fprintf(stderr, "[TMS:%d, TDI:%d, TDO_ARG:%d, TDO_LINE:%d, RMASK:%d, RC:%d]\n", tms, tdi, tdo, line_tdo, rmask, rc);
	}

	u->clockcount++;
	return rc;
}

static void h_pulse_sck(struct libxsvf_host *h)
{
	struct udata_s *u = h->user_data;
	if (u->verbose >= 4) {
		fprintf(stderr, "[SCK]\n");
	}
	io_sck(0);
	io_sck(1);
}

static void h_set_trst(struct libxsvf_host *h, int v)
{
	struct udata_s *u = h->user_data;
	if (u->verbose >= 4) {
		fprintf(stderr, "[TRST:%d]\n", v);
	}
	io_trst(v);
}

static int h_set_frequency(struct libxsvf_host *h, int v)
{
	fprintf(stderr, "WARNING: Setting JTAG clock frequency to %d ignored!\n", v);
	return 0;
}

static void h_report_tapstate(struct libxsvf_host *h)
{
	struct udata_s *u = h->user_data;
	if (u->verbose >= 3) {
		fprintf(stderr, "[%s]\n", libxsvf_state2str(h->tap_state));
	}
}

static void h_report_device(struct libxsvf_host *h, unsigned long idcode)
{
	// struct udata_s *u = h->user_data;
	printf("idcode=0x%08lx, revision=0x%01lx, part=0x%04lx, manufactor=0x%03lx\n", idcode,
			(idcode >> 28) & 0xf, (idcode >> 12) & 0xffff, (idcode >> 1) & 0x7ff);
}

static void h_report_status(struct libxsvf_host *h, const char *message)
{
	struct udata_s *u = h->user_data;
	if (u->verbose >= 2) {
		fprintf(stderr, "[STATUS] %s\n", message);
	}
}

static void h_report_error(struct libxsvf_host *h, const char *file, int line, const char *message)
{
	fprintf(stderr, "[%s:%d] %s\n", file, line, message);
}

static int realloc_maxsize[LIBXSVF_MEM_NUM];

static void *h_realloc(struct libxsvf_host *h, void *ptr, int size, enum libxsvf_mem which)
{
	struct udata_s *u = h->user_data;
	if (size > realloc_maxsize[which])
		realloc_maxsize[which] = size;
	if (u->verbose >= 3) {
		fprintf(stderr, "[REALLOC:%s:%d]\n", libxsvf_mem2str(which), size);
	}
	return realloc(ptr, size);
}

static struct udata_s u;

static struct libxsvf_host h = {
	.udelay = h_udelay,
	.setup = h_setup,
	.shutdown = h_shutdown,
	.getbyte = h_getbyte,
	.pulse_tck = h_pulse_tck,
	.pulse_sck = h_pulse_sck,
	.set_trst = h_set_trst,
	.set_frequency = h_set_frequency,
	.report_tapstate = h_report_tapstate,
	.report_device = h_report_device,
	.report_status = h_report_status,
	.report_error = h_report_error,
	.realloc = h_realloc,
	.user_data = &u
};

const char *progname;

static void copyleft()
{
	static int already_printed = 0;
	if (already_printed)
		return;
	fprintf(stderr, "xsvftool-gpio, part of Lib(X)SVF (http://www.clifford.at/libxsvf/).\n");
	fprintf(stderr, "Copyright (C) 2009  RIEGL Research ForschungsGmbH\n");
	fprintf(stderr, "Copyright (C) 2009  Clifford Wolf <clifford@clifford.at>\n");
	fprintf(stderr, "Lib(X)SVF is free software licensed under the ISC license.\n");
	already_printed = 1;
}

static void help()
{
	copyleft();
	fprintf(stderr, "\n");
	fprintf(stderr, "Usage: %s [ -r funcname ] [ -v ... ] [ -L | -B ] { -s svf-file | -x xsvf-file | -c } ...\n", progname);
	fprintf(stderr, "\n");
	fprintf(stderr, "   -r funcname\n");
	fprintf(stderr, "          Dump C-code for pseudo-allocator based on example files\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "   -v, -vv, -vvv, -vvvv\n");
	fprintf(stderr, "          Verbose, more verbose and even more verbose\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "   -L, -B\n");
	fprintf(stderr, "          Print RMASK bits as hex value (little or big endian)\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "   -s svf-file\n");
	fprintf(stderr, "          Play the specified SVF file\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "   -x xsvf-file\n");
	fprintf(stderr, "          Play the specified XSVF file\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "   -c\n");
	fprintf(stderr, "          List devices in JTAG chain\n");
	fprintf(stderr, "\n");
	exit(1);
}

int main(int argc, char **argv)
{
	int rc = 0;
	int gotaction = 0;
	int hex_mode = 0;
	const char *realloc_name = NULL;
	int opt, i, j;

	progname = argc >= 1 ? argv[0] : "xvsftool";
	while ((opt = getopt(argc, argv, "r:vLBx:s:c")) != -1)
	{
		switch (opt)
		{
		case 'r':
			realloc_name = optarg;
			break;
		case 'v':
			copyleft();
			u.verbose++;
			break;
		case 'x':
		case 's':
			gotaction = 1;
			if (u.verbose)
				fprintf(stderr, "Playing %s file `%s'.\n", opt == 's' ? "SVF" : "XSVF", optarg);
			if (!strcmp(optarg, "-"))
				u.f = stdin;
			else
				u.f = fopen(optarg, "rb");
			if (u.f == NULL) {
				fprintf(stderr, "Can't open %s file `%s': %s\n", opt == 's' ? "SVF" : "XSVF", optarg, strerror(errno));
				rc = 1;
				break;
			}
			if (libxsvf_play(&h, opt == 's' ? LIBXSVF_MODE_SVF : LIBXSVF_MODE_XSVF) < 0) {
				fprintf(stderr, "Error while playing %s file `%s'.\n", opt == 's' ? "SVF" : "XSVF", optarg);
				rc = 1;
			}
			if (strcmp(optarg, "-"))
				fclose(u.f);
			break;
		case 'c':
			gotaction = 1;
			if (libxsvf_play(&h, LIBXSVF_MODE_SCAN) < 0) {
				fprintf(stderr, "Error while scanning JTAG chain.\n");
				rc = 1;
			}
			break;
		case 'L':
			hex_mode = 1;
			break;
		case 'B':
			hex_mode = 2;
			break;
		default:
			help();
			break;
		}
	}

	if (!gotaction)
		help();

	if (u.verbose) {
		fprintf(stderr, "Total number of clock cycles: %d\n", u.clockcount);
		fprintf(stderr, "Number of significant TDI bits: %d\n", u.bitcount_tdi);
		fprintf(stderr, "Number of significant TDO bits: %d\n", u.bitcount_tdo);
		if (rc == 0) {
			fprintf(stderr, "Finished without errors.\n");
		} else {
			fprintf(stderr, "Finished with errors!\n");
		}
	}

	if (u.retval_i) {
		if (hex_mode) {
			printf("0x");
			for (i=0; i < u.retval_i; i+=4) {
				int val = 0;
				for (j=i; j<i+4; j++)
					val = val << 1 | u.retval[hex_mode > 1 ? j : u.retval_i - j - 1];
				printf("%x", val);
			}
		} else {
			printf("%d rmask bits:", u.retval_i);
			for (i=0; i < u.retval_i; i++)
				printf(" %d", u.retval[i]);
		}
		printf("\n");
	}

	if (realloc_name) {
		int num = 0;
		for (i = 0; i < LIBXSVF_MEM_NUM; i++) {
			if (realloc_maxsize[i] > 0)
				num = i+1;
		}
		printf("void *%s(void *h, void *ptr, int size, int which) {\n", realloc_name);
		for (i = 0; i < num; i++) {
			if (realloc_maxsize[i] > 0)
				printf("\tstatic unsigned char buf_%s[%d];\n", libxsvf_mem2str(i), realloc_maxsize[i]);
		}
		printf("\tstatic unsigned char *buflist[%d] = {", num);
		for (i = 0; i < num; i++) {
			if (realloc_maxsize[i] > 0)
				printf("%sbuf_%s", i ? ", " : " ", libxsvf_mem2str(i));
			else
				printf("%s(void*)0", i ? ", " : " ");
		}
		printf(" };\n\tstatic int sizelist[%d] = {", num);
		for (i = 0; i < num; i++) {
			if (realloc_maxsize[i] > 0)
				printf("%ssizeof(buf_%s)", i ? ", " : " ", libxsvf_mem2str(i));
			else
				printf("%s0", i ? ", " : " ");
		}
		printf(" };\n");
		printf("\treturn which < %d && size <= sizelist[which] ? buflist[which] : (void*)0;\n", num);
		printf("};\n");
	}

	return rc;
}
