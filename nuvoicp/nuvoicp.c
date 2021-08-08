/*
 * nuvoicp, a RPi ICP flasher for the Nuvoton N76E003
 * https://github.com/steve-m/N76E003-playground
 *
 * Copyright (c) 2021 Steve Markgraf <steve@steve-m.de>
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <gpiod.h>
#include <string.h>
#include <errno.h>

/* GPIO line numbers for RPi, must be changed for other SBCs */
#define GPIO_DAT	20
#define GPIO_RST	21
#define GPIO_CLK	26

#define N76E003_DEVID	0x3650

#define FLASH_SIZE	(18 * 1024)
#define LDROM_MAX_SIZE	(4 * 1024)

#define APROM_FLASH_ADDR	0x0
#define CFG_FLASH_ADDR		0x30000
#define CFG_FLASH_LEN		5

#define CMD_READ_UID		0x04
#define CMD_READ_CID		0x0b
#define CMD_READ_DEVICE_ID	0x0c
#define CMD_READ_FLASH		0x00
#define CMD_WRITE_FLASH		0x21
#define CMD_MASS_ERASE		0x26
#define CMD_PAGE_ERASE		0x22

#define CONSUMER "nuvoicp"
struct gpiod_chip *chip;
struct gpiod_line *dat_line, *rst_line, *clk_line;

int pgm_init(void)
{
	int ret;

	chip = gpiod_chip_open_by_name("gpiochip0");
	if (!chip) {
		fprintf(stderr, "Open chip failed\n");
		return -ENOENT;
	}

	dat_line = gpiod_chip_get_line(chip, GPIO_DAT);
	rst_line = gpiod_chip_get_line(chip, GPIO_RST);
	clk_line = gpiod_chip_get_line(chip, GPIO_CLK);
	if (!dat_line || !clk_line || !rst_line) {
		fprintf(stderr, "Error getting required GPIO lines!\n");
		return -ENOENT;
	}

	ret = gpiod_line_request_input(dat_line, CONSUMER);
	ret |= gpiod_line_request_output(rst_line, CONSUMER, 0);
	ret |= gpiod_line_request_output(clk_line, CONSUMER, 0);
	if (ret < 0) {
		fprintf(stderr, "Request line as output failed\n");
		return -ENOENT;
	}

	return 0;
}

void pgm_set_dat(int val)
{
	if (gpiod_line_set_value(dat_line, val) < 0)
		fprintf(stderr, "Setting data line failed\n");
}

int pgm_get_dat(void)
{
	int ret = gpiod_line_get_value(dat_line);
	if (ret < 0)
		fprintf(stderr, "Getting data line failed\n");
	return ret;
}

void pgm_set_rst(int val)
{
	if (gpiod_line_set_value(rst_line, val) < 0)
		fprintf(stderr, "Setting reset line failed\n");
}

void pgm_set_clk(int val)
{
	if (gpiod_line_set_value(clk_line, val) < 0)
		fprintf(stderr, "Setting clock line failed\n");
}

void pgm_dat_dir(int state)
{
	gpiod_line_release(dat_line);

	int ret;
	if (state)
		ret = gpiod_line_request_output(dat_line, CONSUMER, 0);
	else
		ret = gpiod_line_request_input(dat_line, CONSUMER);

	if (ret < 0)
		fprintf(stderr, "Setting data directions failed\n");
}

void pgm_deinit(void)
{
	/* release reset */
	pgm_set_rst(1);

	gpiod_chip_close(chip);
}

void icp_bitsend(uint32_t data, int len)
{
	/* configure DAT pin as output */
	pgm_dat_dir(1);

	int i = len;
	while (i--) {
		pgm_set_dat((data >> i) & 1);
		pgm_set_clk(1);
		pgm_set_clk(0);
	}
}

void icp_send_command(uint8_t cmd, uint32_t dat)
{
	icp_bitsend((dat << 6) | cmd, 24);
}

void icp_init(void)
{
	uint32_t icp_seq = 0x9e1cb6;
	int i = 24;

	while (i--) {
		pgm_set_rst((icp_seq >> i) & 1);
		usleep(10000);
	}

	usleep(100);

	icp_bitsend(0x5aa503, 24);
}

void icp_exit(void)
{
	pgm_set_rst(1);
	usleep(5000);
	pgm_set_rst(0);
	usleep(10000);
	icp_bitsend(0xf78f0, 24);
	usleep(500);
	pgm_set_rst(1);
}

uint8_t icp_read_byte(int end)
{
	pgm_dat_dir(0);

	uint8_t data = 0;
	int i = 8;

	while (i--) {
		int state = pgm_get_dat();
		pgm_set_clk(1);
		pgm_set_clk(0);
		data |= (state << i);
	}

	pgm_dat_dir(1);
	pgm_set_dat(end);
	pgm_set_clk(1);
	pgm_set_clk(0);
	pgm_set_dat(0);

	return data;
}

void icp_write_byte(uint8_t data, int end, int delay1, int delay2)
{
	icp_bitsend(data, 8);
	pgm_set_dat(end);
	usleep(delay1);
	pgm_set_clk(1);
	usleep(delay2);
	pgm_set_dat(0);
	pgm_set_clk(0);
}

uint32_t icp_read_device_id(void)
{
	icp_send_command(CMD_READ_DEVICE_ID, 0);

	uint8_t devid[2];
	devid[0] = icp_read_byte(0);
	devid[1] = icp_read_byte(1);

	return (devid[1] << 8) | devid[0];
}

uint8_t icp_read_cid(void)
{
	icp_send_command(CMD_READ_CID, 0);
	return icp_read_byte(1);
}

uint32_t icp_read_uid(void)
{
	uint8_t uid[3];

	for (int i = 0; i < sizeof(uid); i++) {
		icp_send_command(CMD_READ_UID, i);
		uid[i] = icp_read_byte(1);
	}

	return (uid[2] << 16) | (uid[1] << 8) | uid[0];
}

uint32_t icp_read_ucid(void)
{
	uint8_t ucid[4];

	for (int i = 0; i < sizeof(ucid); i++) {
		icp_send_command(CMD_READ_UID, i + 0x20);
		ucid[i] = icp_read_byte(1);
	}

	return (ucid[3] << 24) | (ucid[2] << 16) | (ucid[1] << 8) | ucid[0];
}

uint32_t icp_read_flash(uint32_t addr, uint32_t len, uint8_t *data)
{
	icp_send_command(CMD_READ_FLASH, addr);

	for (int i = 0; i < len; i++)
		data[i] = icp_read_byte(i == (len-1));

	return addr + len;
}

uint32_t icp_write_flash(uint32_t addr, uint32_t len, uint8_t *data)
{
	int progress_printed = 0;
	icp_send_command(CMD_WRITE_FLASH, addr);

	for (int i = 0; i < len; i++) {
		icp_write_byte(data[i], i == (len-1), 200, 50);

		/* print some progress */
		if (((i % 256) == 0) && len > CFG_FLASH_LEN) {
			fprintf(stderr, ".");
			progress_printed++;
		}
	}

	if (progress_printed)
		fprintf(stderr, "\n");

	return addr + len;
}

void icp_dump_config()
{
	uint8_t cfg[CFG_FLASH_LEN];
	icp_read_flash(CFG_FLASH_ADDR, CFG_FLASH_LEN, cfg);

	fprintf(stderr, "MCU Boot select:\t%s\n", cfg[0] & 0x80 ? "APROM" : "LDROM");

	int ldrom_size = (7 - (cfg[1] & 0x7)) * 1024;
	fprintf(stderr, "LDROM size:\t\t%d Bytes\n", ldrom_size);
	fprintf(stderr, "APROM size:\t\t%d Bytes\n", FLASH_SIZE - ldrom_size);
}

void icp_mass_erase(void)
{
	icp_send_command(CMD_MASS_ERASE, 0x3A5A5);
	icp_write_byte(0xff, 1, 100000, 10000);
}

void icp_page_erase(uint32_t addr)
{
	icp_send_command(CMD_PAGE_ERASE, addr);
	icp_write_byte(0xff, 1, 10000, 1000);
}

void usage(void)
{
	fprintf(stderr,
		"nuvoicp, a RPi ICP flasher for the Nuvoton N76E003\n"
		"written by Steve Markgraf <steve@steve-m.de>\n\n"
		"Usage:\n"
		"\t[-r <filename> read entire flash to file]\n"
		"\t[-w <filename> write file to APROM/entire flash (if LDROM is disabled)]\n"
		"\t[-l <filename> write file to LDROM, enable LDROM, enable boot from LDROM]\n"
		"Pinout:\n"
		"         [...]\n"
		"        G19 G16\n"
		"        CLK DAT\n"
		"        GND RST\n"
		"    ________\n"
		"   |   USB  |\n"
		"   |  PORTS |\n"
		"   |________|\n");
	exit(1);
}

int main(int argc, char *argv[])
{
	int opt;
	int write_aprom = 0, write_ldrom = 0;
	int aprom_program_size = 0, ldrom_program_size = 0;
	char *filename = NULL, *filename_ldrom = NULL;
	FILE *file = NULL, *file_ldrom = NULL;
	uint8_t read_data[FLASH_SIZE], write_data[FLASH_SIZE], ldrom_data[LDROM_MAX_SIZE];

	memset(read_data, 0xff, sizeof(read_data));
	memset(write_data, 0xff, sizeof(write_data));
	memset(ldrom_data, 0xff, sizeof(ldrom_data));

	while ((opt = getopt(argc, argv, "r:w:l:")) != -1) {
		switch (opt) {
		case 'r':
			filename = optarg;
			break;
		case 'w':
			filename = optarg;
			write_aprom = 1;
			break;
		case 'l':
			filename_ldrom = optarg;
			write_ldrom = 1;
			break;
		case 'h':
		default:
			usage();
			break;
		}
	}

	if (filename)
		file = fopen(filename, write_aprom ? "rb" : "wb");

	if (filename_ldrom)
		file_ldrom = fopen(filename_ldrom, "rb");

	if (!(file || file_ldrom)) {
		fprintf(stderr, "Failed to open file!\n\n");
		usage();
		goto err;
	}

	if (pgm_init() < 0)
		goto err;

	icp_init();

	uint16_t devid = icp_read_device_id();

	if (devid == N76E003_DEVID)
		fprintf(stderr, "Found N76E003\n");
	else {
		fprintf(stderr, "Unknown Device ID: 0x%04x\n", devid);
		goto out;
	}

	uint8_t cid = icp_read_cid();

	fprintf(stderr,"CID\t\t\t0x%02x\n", cid);
	fprintf(stderr,"UID\t\t\t0x%06x\n", icp_read_uid());
	fprintf(stderr,"UCID\t\t\t0x%08x\n", icp_read_ucid());

	/* Erase entire flash */
	if (write_aprom || write_ldrom)
		icp_mass_erase();

	int chosen_ldrom_sz = 0;

	if (write_ldrom) {
		ldrom_program_size = fread(ldrom_data, 1, LDROM_MAX_SIZE, file_ldrom);
		uint8_t chosen_ldrom_sz_kb = ((ldrom_program_size - 1) / 1024) + 1;
		uint8_t ldrom_sz_cfg = (7 - chosen_ldrom_sz_kb) & 0x7;
		chosen_ldrom_sz = chosen_ldrom_sz_kb * 1024;

		/* configure LDROM size and enable boot from LDROM */
		uint8_t cfg[CFG_FLASH_LEN] = { 0x7f, 0xf8 | ldrom_sz_cfg, 0xff, 0xff, 0xff };
		icp_write_flash(CFG_FLASH_ADDR, CFG_FLASH_LEN, cfg);

		/* program LDROM */
		icp_write_flash(FLASH_SIZE - chosen_ldrom_sz, ldrom_program_size, ldrom_data);
		fprintf(stderr, "Programmed LDROM (%d bytes)\n", ldrom_program_size);
	}

	if (write_aprom) {
		int aprom_size = FLASH_SIZE - chosen_ldrom_sz;
		aprom_program_size = fread(write_data, 1, aprom_size, file);

		/* program flash */
		icp_write_flash(APROM_FLASH_ADDR, aprom_program_size, write_data);
		fprintf(stderr, "Programmed APROM (%d bytes)\n", aprom_program_size);
	}

	icp_dump_config();

	if (write_aprom || write_ldrom) {
		/* verify flash */
		icp_read_flash(APROM_FLASH_ADDR, FLASH_SIZE, read_data);

		/* copy the LDROM content in the buffer of the entire flash for
		 * verification */
		memcpy(&write_data[FLASH_SIZE - chosen_ldrom_sz], ldrom_data, chosen_ldrom_sz);
		if (memcmp(write_data, read_data, FLASH_SIZE))
			fprintf(stderr, "\nError when verifying flash!\n");
		else
			fprintf(stderr, "\nEntire Flash verified successfully!\n");
	} else {
		icp_read_flash(APROM_FLASH_ADDR, FLASH_SIZE, read_data);

		/* save flash content to file */
		if (fwrite(read_data, 1, FLASH_SIZE, file) != FLASH_SIZE)
			fprintf(stderr, "Error writing file!\n");
		else
			fprintf(stderr, "\nFlash successfully read.\n");
	}

out:
	icp_exit();
	pgm_deinit();
	return 0;

err:
	return 1;
}
