/*
 * Copyright (C) 2013 Plastic Logic Limited
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */
/*
 * S1D13541.c -- Epson S1D13541 controller specific functions
 *
 * Authors: Nick Terry <nick.terry@plasticlogic.com>
 *
 */

#include <stddef.h>
#include <stdlib.h>
#include "types.h"
#include "assert.h"
#include "S1D13541.h"
#include "epson-cmd.h"
#include "epson-utils.h"

#define EPSON_CODE_PATH	"bin/Ecode.bin"
#define WAVEFORM_PATH	"display/waveform.bin"

#define KEY		0x12345678
#define KEY_1	0x1234
#define	KEY_2	0x5678

#define GATE_POWER_BEFORE_INIT	1

/* These need changing to use our definitions */
#define TIMEOUT_MS 5000


#define REG_CLOCK_CONFIGURATION			0x0010

#define BF_INIT_CODE_CHECKSUM	(1 << 15)
#define INT_WF_CHECKSUM_ERROR	(1 << 12)
#define INT_WF_INVALID_FORMAT	(1 << 11)
#define INT_WF_CHECKSUM_ERROR	(1 << 12)
#define INT_WF_OVERFLOW			(1 << 13)

/* 541 register bit field value list */
#define INTERNAL_CLOCK_ENABLE		(1 << 7)
#define INIT_CODE_CHECKSUM_ERROR	(0 << 15)

/* 541 constant value list */
#define PRODUCT_CODE			0x0053
#define	WAVEFORM_ADDRESS	0x00080000L

static int wait_for_HRDY_ready(int timeout)
{
	epson_wait_for_idle();
	return 0;
}

static int delay_for_HRDY_ready(int timeout)
{
	epson_wait_for_idle();
	return 0;
}

static int send_init_code()
{
	short readval;
	if (epson_loadEpsonCode(EPSON_CODE_PATH) < 0)
		return -EIO;

	epson_reg_read(CMD_SEQ_AUTOBOOT_CMD_REG, &readval);
	if ((readval & BF_INIT_CODE_CHECKSUM) == INIT_CODE_CHECKSUM_ERROR) {
		printk(KERN_ERR "541: init code checksum error\n");
		return -EIO;
	}

	return 0;
}

static int send_waveform(void)
{
	int retval = 0;
	short readval = 0;

	if (epson_loadEpsonWaveform(WAVEFORM_PATH, WAVEFORM_ADDRESS) < 0)
		return -EIO;

	retval = delay_for_HRDY_ready(TIMEOUT_MS);
	if (retval != 0) {
		printk(KERN_ERR "541: failed to send waveform\n");
		goto end;
	}

	epson_reg_read(DISPLAY_INT_RAW_STAT_REG, &readval);

	if ((readval & INT_WF_INVALID_FORMAT) != 0) {
		printk(KERN_ERR "541: invalid waveform format\n");
		retval = -EIO;
	}
	if ((readval & INT_WF_CHECKSUM_ERROR) != 0) {
		printk(KERN_ERR "541: waveform checksum error\n");
		retval = -EIO;
	}
	if ((readval & INT_WF_OVERFLOW) != 0) {
		printk(KERN_ERR "541: waveform overflow\n");
		retval = -EIO;
	}

end:
	return retval;
}

int s1d13541_send_waveform(void)
{
	return send_waveform();
}

static int chip_init(struct s1d135xx *epson)
{
	int retval = 0;
	short product;

	assert(epson);

	epson_softReset();

	retval = wait_for_HRDY_ready(TIMEOUT_MS);

	epson_reg_read(PROD_CODE_REG, &product);

	printk(KERN_INFO "541: product code = 0x%04x\n", product);
	if (product != PRODUCT_CODE) {
		printk(KERN_ERR "541: invalid product code\n");
		retval = -EIO;
		goto error;
	}

	epson_reg_write(REG_CLOCK_CONFIGURATION, INTERNAL_CLOCK_ENABLE);
	msleep(10);
	retval = wait_for_HRDY_ready(TIMEOUT_MS);
	if (retval != 0) {
		printk(KERN_ERR "541: clock enable failed\n");
		goto error;
	}

	retval = send_init_code();
	if (retval != 0)
		goto error;

	epson_cmd_p0(INIT_SYS_STBY);
	epson->power_mode = PWR_STATE_STANDBY;
	msleep(100);

	retval = wait_for_HRDY_ready(TIMEOUT_MS);
	if (retval != 0) {
		printk(KERN_ERR "541: init and standby failed\n");
		goto error;
	}

	epson_reg_write(REG_PROTECTION_KEY_1, epson->keycode1);
	epson_reg_write(REG_PROTECTION_KEY_2, epson->keycode2);
	retval = wait_for_HRDY_ready(TIMEOUT_MS);
	if (retval != 0) {
		printk(KERN_ERR "541: write keycode failed\n");
		goto error;
	}

	retval = send_waveform();
	if (retval != 0)
		goto error;

#if GATE_POWER_BEFORE_INIT
	epson_mode_run(epson);
	epson_cmd_p0(UPD_GDRV_CLR);
	epson_wait_for_idle();
#else
	epson_cmd_p0(UPD_GDRV_CLR);
	epson_wait_for_idle();
	epson_mode_run(epson);
#endif
	epson_cmd_p0(WAIT_DSPE_TRG);
	retval = wait_for_HRDY_ready(TIMEOUT_MS);
	if (retval != 0) {
		printk(KERN_ERR "541: clear gate driver failed\n");
		goto error;
	}

	// get x and y definitions.
	epson_reg_read(REG_LINE_DATA_LENGTH, &epson->xres);
	epson_reg_read(REG_FRAME_DATA_LENGTH, &epson->yres);

	// init_rot_mode
	// not required in this case
#if 0
	// fill buffer with blank image using H/W fill support
	epson_reg_read(HOST_MEM_CONF_REG, &readval);
	epson_reg_write(HOST_MEM_CONF_REG, (readval &= ~1));
	epson_reg_write(DISPLAY_UPD_BUFF_PXL_VAL_REG, 0x00f0);
	epson_reg_write(DISPLAY_CTRL_TRIG_REG, 3); // trigger buffer init
	epson_cmd_p0(WAIT_DSPE_TRG);
	epson_wait_for_idle(); 		// wait for it to complete

	// update the current pixel data - no display update occurs
	epson_cmd_p0(UPD_INIT);
	epson_wait_for_idle();
	epson_cmd_p0(WAIT_DSPE_TRG);
	epson_wait_for_idle();
#endif

	return 0;

error:
	return retval;

}

/* initialise the pixel buffer but do not drive the display
 *
 */
int s1d13541_init_display(struct s1d135xx *epson)
{
	assert(epson);

	epson_cmd_p0(UPD_INIT);
	epson_wait_for_idle();
	epson_cmd_p0(WAIT_DSPE_TRG);
	epson_wait_for_idle();

	return 0;
}

/* Update the display using the specified waveform
 */
int s1d13541_update_display(struct s1d135xx *epson, int waveform)
{
	assert(epson);

	epson_cmd_p1(UPD_FULL, WAVEFORM_MODE(waveform) | UPDATE_LUT(0));
	epson_wait_for_idle();

	epson_cmd_p0(WAIT_DSPE_TRG);
	epson_wait_for_idle();

	epson_cmd_p0(WAIT_DSPE_FREND);
	epson_wait_for_idle();

	return 0;
}

/* Initialise the 541 controller and leave it in a state ready to do updates
 */
int s1d13541_init(screen_t screen, struct s1d135xx **controller)
{
	int ret;
	screen_t previous;
	struct s1d135xx *epson;

	*controller = epson = (struct s1d135xx *)malloc(sizeof(struct s1d135xx));
	if (NULL == epson)
		return -ENOMEM;

	epson_wait_for_idle_mask(0x2000, 0x2000);

	epson->screen = screen;

	epson->keycode1 = KEY_2;
	epson->keycode2 = KEY_1;
	epson->temp_mode = TEMP_MODE_UNDEFINED;
	epson->power_mode = PWR_STATE_UNDEFINED;


	if (epsonif_claim(0, screen, &previous) < 0)
		return -EIO;

	ret = chip_init(epson);

	epsonif_release(0, previous);

	return ret;
}

/* Configure controller for specified temperature mode */
int s1d13541_set_temperature_mode(struct s1d135xx *epson, short temp_mode)
{
	short reg;

	assert(epson);

	epson_reg_read(PERIPHERAL_CONFIG_REG, &reg);

	reg &= ~TEMP_SENSOR_CONTROL;

	switch(temp_mode)
	{
	case TEMP_MODE_MANUAL:
		break;
	case TEMP_MODE_INTERNAL:
		reg &= ~TEMP_SENSOR_EXTERNAL;
		break;
	case TEMP_MODE_EXTERNAL:
		reg |= TEMP_SENSOR_EXTERNAL;
		break;
	case TEMP_MODE_UNDEFINED:
	default:
		return -EPARAM;
	}

	epson_reg_write(PERIPHERAL_CONFIG_REG, reg );

	epson->temp_mode = temp_mode;

	/* Configure the controller to check for waveform update after temperature sense
	 */
	epson_reg_read(REG_WAVEFORM_DECODER_BYPASS, &reg);
	epson_reg_write(REG_WAVEFORM_DECODER_BYPASS, (reg | AUTO_TEMP_JUDGE_ENABLE));

	return 0;
}

int s1d13541_set_temperature(struct s1d135xx *epson, s8 temp)
{
	assert(epson);

	epson->temp_set = temp;
	return 0;
}

int s1d13541_get_temperature(struct s1d135xx *epson, s8 *temp)
{
	assert(epson);

	*temp = epson->temp_measured;
	return 0;
}

#define	GENERIC_TEMP_CONF_REG	0x057E
#define	DISPLAY_INT_WAVEFORM_UPDATE	0x4000

static void measured_temp(short temp_reg, u8 *needs_update, s8 *measured)
{
	short reg;

	epson_reg_read(DISPLAY_INT_RAW_STAT_REG, &reg);
	epson_reg_write(DISPLAY_INT_RAW_STAT_REG, DISPLAY_INT_WAVEFORM_UPDATE | DISPLAY_INT_TEMP_OUT_OF_RANGE);
	*needs_update = (reg & DISPLAY_INT_WAVEFORM_UPDATE) ? 1 : 0;

	epson_reg_read(temp_reg, &reg);
	*measured = (s8)(reg & 0x00ff);
}

/* measure temperature using specified mode */
int s1d13541_measure_temperature(struct s1d135xx *epson, u8 *needs_update)
{
	s8 temp_measured;

	assert(epson);

	switch(epson->temp_mode)
	{
	case TEMP_MODE_UNDEFINED:
	default:
		return -1;

	case TEMP_MODE_MANUAL:
		// apply manually specified temperature
		epson_reg_write(GENERIC_TEMP_CONF_REG, 0xC000 | (epson->temp_set & 0x00ff));
		epson_wait_for_idle();
		measured_temp(GENERIC_TEMP_CONF_REG, needs_update, &temp_measured);
		break;
	case TEMP_MODE_EXTERNAL:
		// check temperature sensor is configured
		epson_mode_standby(epson);
		epson_cmd_p0(RD_TEMP);
		epson_wait_for_idle();
		epson_mode_run(epson);
		measured_temp(0x0216, needs_update, &temp_measured);
		break;
	case TEMP_MODE_INTERNAL:
		// use the internal temperature sensor
		epson_mode_standby(epson);
		epson_cmd_p0(RD_TEMP);
		epson_wait_for_idle();
		epson_mode_run(epson);
		measured_temp(0x0576, needs_update, &temp_measured);
		break;
	}

	epson->temp_measured = temp_measured;

	printk("541: Temperature: %d\n", temp_measured);

	return 0;
}