/* This is a SPI device driver for a low power transceiver chip developed at
 * the IAS, RWTH Aachen to support the IEEE 802.15.4 standard. The code design
 * is oriented at the driver for the AT86RF230 chip from Atmel.
 * IAS LPRF driver
 *
 * Copyright (C) 2015 IAS RWTH Aachen
 * Adapted from  AT86RF230 driver, Copyright (C) 2009-2012 Siemens AG
 * Also based on scull driver from "Linux Device Drivers"
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details
 *
 * Works on kernel version 4.4.15+
 *
 * Written by:
 * Tilman Sinning <tilman.sinning@rwth-aachen.de>
 * Moritz Schrey <mschrey@ias.rwth-aachen.de>
 * Jan Richter-Brockmann <jan.richter-brockmann@rwth-aachen.de>
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/hrtimer.h>
#include <linux/wait.h>
#include <linux/mutex.h>
#include <linux/gpio.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/spi/spi.h>
#include <linux/regmap.h>
#include <linux/of_gpio.h>
#include <linux/ieee802154.h>
#include <linux/debugfs.h>
#include <linux/kfifo.h>

#include <net/mac802154.h>
#include <net/cfg802154.h>

#include "lprf.h"
#include "lprf_registers.h"

struct lprf_local;
struct lprf_state_change;


/***
 *      ____   _                       _
 *     / ___| | |_  _ __  _   _   ___ | |_  ___
 *     \___ \ | __|| '__|| | | | / __|| __|/ __|
 *      ___) || |_ | |   | |_| || (__ | |_ \__ \
 *     |____/  \__||_|    \__,_| \___| \__||___/
 *
 * This Section contains some struct declarations that are needed
 * for the driver.
 */

/**
 * lprf_phy_status is needed for getting asynchronous phy_status
 * information from the chip.
 *
 * @spi_device: spi device to use
 * @spi_message: spi_message for async spi transfer
 * @rx_buf: rx buffer for spi communication
 * @tx_buf: tx buffer for spi communication
 * @is_active: variable used for synchronization to avoid starting a status
 * 	read before the last status read finished. That could lead to data
 * 	corruption since the same spi messages and buffers would be used.
 *
 * The LPRF Chip supports to get physical status information by reading
 * just one byte from the SPI interface. Therefore getting the status
 * informations is different from a normal register access. To avoid
 * mixing up register access and phy_status information with potential
 * issues due to bad synchronization and data reuse this struct is designated
 * especially to reading phy_status information.
 * Also have a look at lprf_phy_status_async().
 */
struct lprf_phy_status {
        struct spi_device *spi_device;
        struct spi_message spi_message;
        struct spi_transfer spi_transfer;
        uint8_t rx_buf[1];
        uint8_t tx_buf[1];
        atomic_t is_active;
};

/**
 * lprf_state_change is a struct used for asynchronous state changes.
 *
 * @lprf: Pointer to lprf_local struct
 * @spi_device: spi device to use for spi transfers
 * @spi_message: spi_message to use for spi transfers
 * @rx_buf: rx buffer for spi communication related to state changes
 * @tx_buf: tx buffer for spi communication related to state changes
 * @to_state: state to change to
 * @transistion_in_progress: variable used for synchronization to make sure
 * 	to complete one state change before initiating another state change
 * @tx_complete: Used to detect when transmitting data finished and
 * 	ieee802154_xmit_complete() can be called.
 * @sm_main_value: Cached value of the SM_MAIN register to enable writing
 * 	 to sub registers without reading the register first.
 * @dem_main_value: Cached value of the DEM_MAIN register to enable writing
 * 	 to sub registers without reading the register first.
 *
 * This struct contains data specifically needed for state changes.
 * This includes particularly SPI data like rx and tx buffers as well as
 * synchronization specific data.
 */
struct lprf_state_change {
	struct lprf_local *lprf;

        struct spi_message spi_message;
        struct spi_transfer spi_transfer;
        uint8_t rx_buf[MAX_SPI_BUFFER_SIZE];
        uint8_t tx_buf[MAX_SPI_BUFFER_SIZE];

        uint8_t to_state;
        atomic_t transition_in_progress;
        bool tx_complete;

        uint8_t sm_main_value;
        uint8_t dem_main_value;
};

/**
 * lprf_local contains general information about the lprf chip.
 *
 * @spi_device: pointer to the spi device the chip is registered to.
 * @regmap: pointer to the regmap structure needed for synchronous
 * 	register access
 * @my_char_dev: char device for the char driver interface.
 * @rx_polling_timer: timer used for polling the chip, as the chip does not
 * 	support an interrupt pin.
 * @hw: ieee802154_hw the chip is registered to.
 * @rx_polling_active: used for disabling the chip polling
 * @phy_status: phy_status struct (see above)
 * @state_change: state_change struct (see above)
 * @tx_skb: Socket buffer containing pending TX data
 * @free_skb: True if tx_skb got allocated by char driver interface and
 * 	needs to be deleted after transmission.
 *
 * This struct exists once per chip and gets allocated in the probe function
 * that handles all the hardware initialization. It contains all relevant
 * information for the lprf chip.
 */
struct lprf_local {
	struct spi_device *spi_device;
	struct regmap *regmap;
	struct cdev my_char_dev;
	struct hrtimer rx_polling_timer;
	struct ieee802154_hw *hw;
	atomic_t rx_polling_active;

	struct lprf_phy_status phy_status;
	struct lprf_state_change state_change;

	struct sk_buff *tx_skb;
	bool free_skb;
};

/**
 * lprf_char_driver_interface is a struct used for the implementation of
 * the char driver interface
 *
 * @is_open: true if the device file is currently opened by as user space
 * 	application.
 * @is_ready: true if is_open and all char driver initializations completed
 * @data_buffer: buffer containing rx data for the char driver interface
 * @wait_for_rx_data: wait queue to wait for rx data to become available
 * @wait_for_tx_ready: wait queue to wait for the chip to get ready for
 * 	tx mode.
 *
 * This struct contains data needed for the char driver interface. The char
 * driver interface is actually not needed for normal chip operation but
 * only used for debugging purposes. This way user space applications are able
 * to read and write raw data to the chip without using the hole IEEE 802.15.4
 * stack.
 */
struct lprf_char_driver_interface {
	atomic_t is_open;
	atomic_t is_ready;
	DECLARE_KFIFO_PTR(data_buffer, uint8_t);
	wait_queue_head_t wait_for_rx_data;
	wait_queue_head_t wait_for_tx_ready;

} lprf_char_driver_interface;


/***
 *      ____   ____  ___      _
 *     / ___| |  _ \|_ _|    / \    ___  ___  ___  ___  ___
 *     \___ \ | |_) || |    / _ \  / __|/ __|/ _ \/ __|/ __|
 *      ___) ||  __/ | |   / ___ \| (__| (__|  __/\__ \\__ \
 *     |____/ |_|   |___| /_/   \_\\___|\___|\___||___/|___/
 *
 * The following sections contains functions to access the ship via SPI.
 * This includes reading from and writing to registers as well as frame
 * read/write access and status information. Synchronous SPI access is handled
 * by the regmap functionality of the linux kernel. Asynchronous SPI access
 * is directly handled by asynchronous spi transfers.
 */

/**
 * __lprf_write writes one register synchronously.
 *
 * @lprf: lprf_local struct
 * @address: 8 bit address of the register
 * @value: new value of the register
 *
 * Typically lprf_write_subreg() is used instead of calling this function
 * directly. Internally the regmap functionality is used.
 */
static inline int
__lprf_write(struct lprf_local *lprf, unsigned int address, unsigned int value)
{
	int ret = 0;
	ret = regmap_write(lprf->regmap, address, value);
	return ret;
}

/**
 * __lprf_read reads value of one register synchronously.
 *
 * @lprf: lprf_locae struct
 * @address: 8 bit register address to read from
 * @value: variable to store the read value in
 *
 * Typically lprf_read_subreg() is used instead of directly calling this
 * function. Internally regmap is used for the register access.
 */
static inline int
__lprf_read(struct lprf_local *lprf, unsigned int address, unsigned int *value)
{
	int ret = 0;
	ret = regmap_read(lprf->regmap, address, value);
	return ret;
}

/**
 * lprf_read_subreg reads the value of a subregister synchronously
 *
 * @lprf: lprf_local struct
 * @addr: address of the register to read from
 * @mask: mask for the sub register to read from
 * @shift: shift needed to align data with LSB
 * @data: variable to store value in
 */
static inline int lprf_read_subreg(struct lprf_local *lprf,
		unsigned int addr, unsigned int mask,
		unsigned int shift, unsigned int *data)
{
	int ret = 0;
	ret = __lprf_read(lprf, addr, data);
	*data = (*data & mask) >> shift;
	return ret;
}

/**
 * lprf_write_subreg writes a value to a subregister synchronously
 *
 * @lprf: lprf_local struct
 * @addr: address of the register to read from
 * @mask: mask for the sub register to read from
 * @shift: shift needed to align data with LSB
 * @data: value to write to subregister
 */
static inline int lprf_write_subreg(struct lprf_local *lprf,
		unsigned int addr, unsigned int mask,
		unsigned int shift, unsigned int data)
{
	return regmap_update_bits(lprf->regmap, addr, mask, data << shift);
}

/**
 * lprf_read_phy_status reads phy_status synchronously by using
 * the spi_read function.
 */
static inline int lprf_read_phy_status(struct lprf_local *lprf)
{
	uint8_t rx_buf[] = {0};
	int ret = 0;

	ret = spi_read(lprf->spi_device, rx_buf, sizeof(rx_buf));
	if (ret)
		return ret;

	return rx_buf[0];

}

/**
 * returns true if the given register is writable. Needed for the regmap
 * caching functionality.
 */
static bool lprf_reg_writeable(struct device *dev, unsigned int reg)
{
	if(((reg >= 0) && (reg < 53)) ||
			((reg >= 56) && (reg < 70)) ||
			((reg >= 80) && (reg < 176)) ||
			((reg >= 192) && (reg <= 243)))
		return true;
	else
		return false;
}

/**
 * returns true if the given register is read only. Needed for the regmap
 * caching functionality.
 */
static bool lprf_is_read_only_reg(unsigned int reg)
{
	switch (reg) {
	case RG_PLL_TPM_GAIN_OUT_L:
	case RG_PLL_TPM_GAIN_OUT_M:
	case RG_PLL_TPM_GAIN_OUT_H:
	case RG_DEM_PD_OUT:
	case RG_DEM_GC_AOUT:
	case RG_DEM_GC_BOUT:
	case RG_DEM_GC_COUT:
	case RG_DEM_GC_DOUT:
	case RG_DEM_FREQ_OFFSET_OUT:
	case RG_SM_STATE:
	case RG_SM_FIFO:
	case RG_SM_GLOBAL:
	case RG_SM_POWER:
	case RG_SM_RX:
	case RG_SM_WAKEUP_EN:
	case RG_SM_DEM_ADC:
	case RG_SM_PLL_TX:
	case RG_SM_PLL_CHAN_INT:
	case RG_SM_PLL_CHAN_FRAC_H:
	case RG_SM_PLL_CHAN_FRAC_M:
	case RG_SM_PLL_CHAN_FRAC_L:
	case RG_SM_TX433:
	case RG_SM_TX800:
	case RG_SM_TX24:
		return true;
	default:
		return false;
	}
}

/**
 * returns true if the given register is readable. Needed for the regmap
 * caching functionality.
 */
static bool lprf_reg_readable(struct device *dev, unsigned int reg)
{

	return lprf_reg_writeable(dev, reg) ||
			lprf_is_read_only_reg(reg);
}

/**
 * returns true if the given register is volatile and therefore can not be
 * cached.
 */
static bool lprf_reg_volatile(struct device *dev, unsigned int reg)
{
	/* All Read Only Registers are volatile */
	if (lprf_is_read_only_reg(reg))
		return true;

	switch (reg) {
	case RG_GLOBAL_RESETB:
	case RG_GLOBAL_initALL:
	case RG_ACTIVATE_ALL:
		return true;
	default:
		return false;
	}
}

/**
 * returns true if the given register is precious and reading that register
 * has side effects (like a clear-on-read flag). The lprf chip has no
 * precious registers.
 */
static bool lprf_reg_precious(struct device *dev, unsigned int reg)
{
	/* The LPRF-Chip has no precious register */
	return false;
}

/**
 * Configuration struct for the regmap functionality. The commands for
 * read and write access are specified here.
 */
static const struct regmap_config lprf_regmap_spi_config = {
	.reg_bits = 16,
	.reg_stride = 1,
	.pad_bits = 0,
	.val_bits = 8,
	.read_flag_mask = 0x80,
	.write_flag_mask = 0xc0,
	.fast_io = 0, /* use spinlock instead of mutex for locking */
	.max_register = 0xF3,
	.use_single_rw = 1, /* we do not support bulk read write */
	.can_multi_write = 0,
	.cache_type = REGCACHE_RBTREE,
	.writeable_reg = lprf_reg_writeable,
	.readable_reg = lprf_reg_readable,
	.volatile_reg = lprf_reg_volatile,
	.precious_reg = lprf_reg_precious,
};

static void lprf_async_write_register(struct lprf_state_change *state_change,
		uint8_t address, uint8_t value,
		void (*complete)(void *context));
static int lprf_phy_status_async(struct lprf_phy_status *phy_status);
static inline void lprf_stop_polling(struct lprf_local *lprf);

/**
 * Call back for asynchronous error recovery. See lprf_async_error().
 */
static void lprf_async_error_recover_callback(void *context)
{
	struct lprf_local *lprf = context;
	atomic_set(&lprf->rx_polling_active, 1);
	lprf_phy_status_async(&lprf->phy_status);
}

/**
 * Call back for asynchronous error recovery. See lprf_async_error().
 */
static void lprf_async_error_recover(void *context)
{
	struct lprf_local *lprf = context;
	struct lprf_state_change *state_change = &lprf->state_change;

	lprf_async_write_register(state_change, RG_GLOBAL_RESETB, 0xff,
			lprf_async_error_recover_callback);
}

/**
 * Resets the chip after an async error has been received.
 *
 * @lprf: lprf_local struct
 * @lprf_state_change: state change struct for the current state change
 * @rc: error code returned by spi_async
 *
 * If spi async returns with an error this function can be used to reset
 * the chip asynchronously and get the chip into normal operation again.
 * Normally there should be no async spi error, so this function will
 * normally not be used at all.
 */
static inline void lprf_async_error(struct lprf_local *lprf,
		struct lprf_state_change *state_change, int rc)
{
	dev_err(&lprf->spi_device->dev, "spi_async error %d\n", rc);
	lprf_stop_polling(lprf);
	lprf_async_write_register(state_change,
			RG_GLOBAL_RESETB, 0, lprf_async_error_recover);
}

/**
 * writes the value of one register asynchronously
 *
 * @state_change: current state change struct
 * @address: 8 bit register address to write the value to
 * @value: value to write to the register
 * @complete: completion callback to call after register access completed.
 * 	Note that the callback function will be called in interrupt context
 * 	as it is directly the callback function of spi_async().
 *
 * Transmitting data and changing the states of the chip needs to be done
 * asynchronously. However, the regmap functionality provides no way to
 * access registers asynchronously with using callback functions at the same
 * time. Therefore an asynchronous way of setting registers is needed. This
 * is done by directly calling spi_async(). See also lprf_async_write_subreg().
 */
static void lprf_async_write_register(struct lprf_state_change *state_change,
		uint8_t address, uint8_t value,
		void (*complete)(void *context))
{
	int ret = 0;
	state_change->tx_buf[0] = REGW;
	state_change->tx_buf[1] = address;
	state_change->tx_buf[2] = value;
	state_change->spi_transfer.len = 3;
	state_change->spi_message.complete = complete;
	ret = spi_async(state_change->lprf->spi_device,
			&state_change->spi_message);
	if (ret)
		lprf_async_error(state_change->lprf, state_change, ret);
}

/**
 * Writes a sub register asynchronously
 *
 * @lprf_state_change: current state change struct
 * @cached value: cached value of the hole register
 * @addr: 8 bit address of the register to write to
 * @mask: sub register mask
 * @shift: shift needed to align sub register with LSB
 * @data: data to write to sub register
 * @complete: completion callback to call after register access completed.
 * 	Note that the callback function will be called in interrupt context
 * 	as it is directly the callback function of spi_async().
 *
 * The use of spi_async instead of regmap for asynchronous register access
 * has the disadvantage that no automatic caching of register data is performed.
 * To avoid reading the register every time a cached value of the register
 * needs to be provided. Typically it should be enough to save the
 * configuration value once after the initial configuration in the probe
 * function.
 * The AR86RF230 needs to access sub registers only during the initial
 * configuration and not during regular operation. Therefore it does not have
 * the problem of accessing sub registers asynchronously and needing to
 * cache those register values manually.
 */
static void
lprf_async_write_subreg(struct lprf_state_change *state_change,
		uint8_t cached_val, uint8_t addr, uint8_t mask, uint8_t shift,
		uint8_t data, void (*complete)(void *context))
{
	uint8_t reg_val = (cached_val & ~mask) | (data << shift);
	lprf_async_write_register(state_change, addr, reg_val, complete);
}

static void lprf_evaluate_phy_status(struct lprf_local *lprf,
		struct lprf_state_change *state_change, uint8_t phy_status);

static void lprf_phy_status_complete(void *context)
{
	struct lprf_local *lprf = context;
	struct lprf_state_change *state_change = &lprf->state_change;
	struct lprf_phy_status *phy_status = &lprf->phy_status;
	uint8_t status = phy_status->rx_buf[0];
	atomic_dec(&lprf->phy_status.is_active);
	lprf_evaluate_phy_status(lprf, state_change, status);
}

/**
 * reads the physical status of the chip
 *
 * @phy_status: lprf_phy_status struct
 *
 * every action like performing a state change or reading data from the chip
 * depends on the physical status of the chip. This functions reads the
 * physical status asynchronously and determines the action to do in the
 * callback. As the lprf chip does not support interrupt pins this function
 * is typically called from within a polling timer callback. It can also be
 * called due to other events like available TX data. As the action to do
 * is completely determined by the physical status of the chip no explicit
 * callback function is needed here.
 */
static int lprf_phy_status_async(struct lprf_phy_status *phy_status)
{
	int ret = 0;
	struct lprf_local *lprf = container_of(
			phy_status, struct lprf_local, phy_status);

	if (atomic_inc_return(&phy_status->is_active) != 1) {
		atomic_dec(&phy_status->is_active);
		return -EBUSY;
	}

	phy_status->spi_message.complete = lprf_phy_status_complete;
	ret = spi_async(phy_status->spi_device, &phy_status->spi_message);
	if (ret)
		lprf_async_error(lprf, &lprf->state_change, ret);
	return 0;
}


/***
 *       ____        _               _         _    _
 *      / ___| __ _ | |  ___  _   _ | |  __ _ | |_ (_)  ___   _ __   ___
 *     | |    / _` || | / __|| | | || | / _` || __|| | / _ \ | '_ \ / __|
 *     | |___| (_| || || (__ | |_| || || (_| || |_ | || (_) || | | |\__ \
 *      \____|\__,_||_| \___| \__,_||_| \__,_| \__||_| \___/ |_| |_||___/
 *
 * This section contains functions with calculations specific for the lprf
 * chip like PLL divisor values or VCO tune values. They are mostly more
 * workarounds for functionality that industry chips like the AT86RF230
 * would typically implement on chip, but are not natively supported by
 * the lprf chip.
 */

/**
 * Calculates the RX Length counter based on the data rate and frame_length
 * (assuming 32MHz clock speed on chip)
 *
 * @kbitrate: over the air data rate in kb/s
 * @frame_length: maximum frame length in bytes. This is the number of bytes
 * 	that will be received by the chip for every RX frame independent
 * 	of the actual length of the specific frame.
 */
static inline int get_rx_length_counter(int kbit_rate, int frame_length)
{
	const int chip_speed_kHz = 32000;
	return 8 * frame_length * chip_speed_kHz / kbit_rate +
			4 * chip_speed_kHz / kbit_rate;
}

/**
 * Reverses the bit order of one byte. This is needed because the lprf chip
 * send rx data with inversed bit order compared to the over-the-air bit
 * order
 */
static inline void reverse_bit_order(uint8_t *byte)
{
	*byte = ((*byte & 0xaa) >> 1) | ((*byte & 0x55) << 1);
	*byte = ((*byte & 0xcc) >> 2) | ((*byte & 0x33) << 2);
	*byte = (*byte >> 4) | (*byte << 4);
}

/*
 * Calculates a proper vco tune value, that is needed for the pll.
 * The vco tune value is dependent on the pll frequency and therefore
 * needs to be changed every time the pll frequency gets changed.
 *
 * Returns the vco tune value or zero for invalid channel_number
 */
static int calc_vco_tune(int channel_number)
{
	switch(channel_number) {
	case 11: return 237;
	case 12: return 235;
	case 13: return 234;
	case 14: return 232;
	case 15: return 231;
	case 16: return 223;
	case 17: return 222;
	case 18: return 220;
	case 19: return 213;
	case 20: return 212;
	case 21: return 210;
	case 22: return 209;
	case 23: return 207;
	case 24: return 206;
	case 25: return 206;
	case 26: return 204;
	default: return 0;
	}
}

/*
 * Calculates the channel center frequency from the channel number as
 * specified in the IEEE 802.15.4 standard. The channel page is assumed
 * to be zero. Returns the frequency in HZ or zero for invalid channel number.
 */
static inline uint32_t calculate_rf_center_freq(int channel_number)
{
	if (channel_number >= 11 && channel_number <= 26) {
		uint32_t f_rf_MHz = 2405 + 5 * (channel_number - 11);
		return f_rf_MHz * 1000000U;
	}

	/* TODO 800MHz support */

	return 0;
}

/*
 * Calculates the PLL values from the rf_frequency and the
 * if_frequency. The rf_frequency can be calculated with
 * calculate_rf_center_freq(). The if_frequency should usually be
 * 1000000 for RX case and zero for TX case.
 *
 * Returns zero on success or -EINVAL for invalid parameters.
 */
static int lprf_calculate_pll_values(uint32_t rf_frequency,
		uint32_t if_frequency,
		int *int_val, int *frac_val)
{
	uint32_t f_lo = 0;
	int frac_correction = 0;

	/*2.4 GHz Frontend*/
	if (rf_frequency > 2000000000) {
		f_lo = ( rf_frequency - if_frequency) / 3 * 2;
		*int_val = f_lo  / 16000000;

		/*
		 * The exact formula would actually be
		 * frac = (f_lo % 16MHz) * 65536 / 1MHz.
		 * However, this would result in an integer overflow
		 * and the linux system seems not to support 64bit
		 * modulo operation. (228/3479) is the smallest error
		 * possible without integer overflow.
		 */
		*frac_val = (f_lo % 16000000) * 228 / 3479 + frac_correction;
		return 0;
	}

	/* TODO 800MHz support */

	return -EINVAL;
}


/***
 *      ____   _          _
 *     / ___| | |_  __ _ | |_  ___  ___
 *     \___ \ | __|/ _` || __|/ _ \/ __|
 *      ___) || |_| (_| || |_|  __/\__ \
 *     |____/  \__|\__,_| \__|\___||___/
 *
 * The following sections contains all function related to state changes and
 * data receiving/sending.
 *
 * As the lprf chip does not support interrupt pins the status of the chip
 * has to be determined regularly by polling. This is done in lprf_start_poll().
 * Before every action like changing a state, receiving or sending data the
 * physical status of the chip will be asked lprf_phy_status_async().
 * In lprf_evaluate_phy_status() the action to be performed will be determined
 * and initiated.
 */

/**
 * starts the timer to poll the status of the chip after the specified time
 * interval has passed.
 *
 * @lprf: lprf_local struct
 * @interval: time interval for the polling timer
 *
 * This function will start a timer. After the given interval lprf_start_poll()
 * will be called. The timer will only be started if lprf.rx_polling_active
 * is true.
 */
static inline void lprf_start_polling_timer(struct lprf_local *lprf,
		ktime_t interval)
{
	if (atomic_read(&lprf->rx_polling_active))
		hrtimer_start(&lprf->rx_polling_timer, interval,
				HRTIMER_MODE_REL);
}

/**
 * sets lprf.rx_polling_active to false to avoid any atimer restarts and
 * cancels any running timer.
 */
static inline void lprf_stop_polling(struct lprf_local *lprf)
{
	atomic_set(&lprf->rx_polling_active, 0);
	hrtimer_cancel(&lprf->rx_polling_timer);
	PRINT_KRIT("RX Data Polling stopped.");
}

/**
 * Starts the polling of physical status information by executing
 * lprf_phy_status_async(). This function is typically called as a timer
 * callback in interrupt context. In future chip versions that support
 * interrupt pins this function can be called directly from the chip
 * interrupt.
 */
static enum hrtimer_restart lprf_start_poll(struct hrtimer *timer)
{
	int rc = 0;
	struct lprf_local *lprf = container_of(
			timer, struct lprf_local, rx_polling_timer);

	rc = lprf_phy_status_async(&lprf->phy_status);
	if (rc)
		PRINT_KRIT("PHY_STATUS BUSY...");
	return HRTIMER_NORESTART;
}

/**
 * Calls ieee802154_xmit_complete() to signal the IEEE 802.15.4 stack that
 * the data transmission completed successfully and the chip is ready for
 * new data. This function needs to be called, after the chip completed
 * transmitting data and changed back to sleep mode or rx mode.
 */
static void lprf_tx_complete(struct lprf_local *lprf)
{
	struct sk_buff *skb_temp = lprf->tx_skb;
	lprf->tx_skb = 0;
	if (lprf->free_skb) /* Data from char driver */
		kfree_skb(skb_temp);
	else /* IEEE 802.15.4 data */
		ieee802154_xmit_complete(lprf->hw, skb_temp, false);
	lprf->free_skb = false;
	lprf->state_change.tx_complete = false;
	wake_up(&lprf_char_driver_interface.wait_for_tx_ready);
	PRINT_KRIT("TX data send successfully");
}

/**
 * Is called after the transition to TX mode completed successfully. Note
 * that the chip is still in TX mode and busy sending data when this
 * function is called. The complete transmission of data is only finished
 * when the chip has changed back to sleep mode or rx mode and
 * lprf_tx_complete() is called.
 */
static void lprf_tx_change_complete(void *context)
{
	struct lprf_local *lprf = context;
	struct lprf_state_change *state_change = &lprf->state_change;

	state_change->tx_complete = true;
	atomic_dec(&lprf->state_change.transition_in_progress);

	lprf_start_polling_timer(lprf, TX_RX_INTERVAL);
}

/**
 * Callback of the frame write function. Initiates the state change to
 * TX mode.
 */
static void __lprf_frame_write_complete(void *context)
{
	struct lprf_local *lprf = context;
	struct lprf_state_change *state_change = &lprf->state_change;

	PRINT_KRIT("Spi Frame Write completed");

	lprf_async_write_subreg(state_change, state_change->sm_main_value,
			SR_SM_COMMAND, STATE_CMD_TX, lprf_tx_change_complete);

	PRINT_KRIT("Change state to TX");
}

/**
 * Starts a frame write via SPI. The Chip should be in sleep mode and otherwise
 * ready for sending data (see lprf_resets()).
 */
static int lprf_start_frame_write(struct lprf_local *lprf)
{
	int ret = 0;
	int i;
	int payload_length = 0;
	int frame_length = 0;
	int shr_index, phr_index, payload_index;
	struct lprf_state_change *state_change = &lprf->state_change;

	payload_length = lprf->tx_skb->len;
	frame_length = sizeof(SYNC_HEADER) +
			PHY_HEADER_LENGTH +
			payload_length;

	shr_index = 2;
	phr_index = shr_index + sizeof(SYNC_HEADER);
	payload_index = phr_index + PHY_HEADER_LENGTH;

	state_change->tx_buf[0] = FRMW;
	state_change->tx_buf[1] = frame_length;

	memcpy(state_change->tx_buf + shr_index,
			SYNC_HEADER, sizeof(SYNC_HEADER));

	state_change->tx_buf[phr_index] = payload_length;

	memcpy(state_change->tx_buf + payload_index,
			lprf->tx_skb->data, lprf->tx_skb->len);

	for(i = 0; i < frame_length; ++i)
		reverse_bit_order(&state_change->tx_buf[shr_index + i]);

	state_change->spi_message.complete = __lprf_frame_write_complete;
	state_change->spi_transfer.len = frame_length + 2;

	ret = spi_async(lprf->spi_device, &state_change->spi_message);
	if (ret)
		PRINT_KRIT("Async_spi returned with error code %d", ret);
	return 0;
}

/**
 * callback function of CMD_RX command. Called after the chip successfully
 * changed to RX mode.
 */
void lprf_rx_change_complete(void *context)
{
	struct lprf_local *lprf = context;
	struct lprf_state_change *state_change = &lprf->state_change;

	atomic_dec(&state_change->transition_in_progress);

	lprf_start_polling_timer(lprf, RX_RX_INTERVAL);
}

/**
 * Compares number_of_bits bits starting from the LSB and returns
 * the number of equal bits. This is needed to find the start of frame
 * delimiter of received data.
 */
static inline int number_of_equal_bits(uint32_t x1, uint32_t x2,
		int number_of_bits)
{
	int i = 0;
	int counter = 0;
	uint32_t combined = ~(x1 ^ x2);

	for (i = 0; i < number_of_bits; ++i) {
		counter += combined & 1;
		combined >>= 1;
	}
	return counter;
}

/**
 * Calculates the data shift from the start of frame delimiter
 *
 * @data: rx_data received from chip. Bit order and polarity needs to be
 * 	already corrected
 * @data_length: number of bytes in data.
 * @uint8_t sfd: start of frame delimiter, i.e. 0xe5
 * @preamble_length: number of octets in the preamble. The maximum supported
 * 	length is 4 octets
 * @is_first_preamble_bit_one: For FSK-modulation based data transfers
 * 	there are basically two types of preambles 010101... and 101010...
 * 	This parameter selects the type currently used.
 *
 * Returns the shift value (6/7/8) or zero if sfd was not found.
 *
 * The lprf chip has a hardware preamble detection. However, the hardware
 * preamble detection is limited to a 8 bit preamble (or optionally 5 bit). In
 * the IEEE 802.15.4 standard the preamble has a length of 4 byte.
 * Additionally the data from the chip is sometimes misaligned by one bit.
 * Therefore the additional preamble bits have to be removed and the
 * misalignment has to be adjusted in software.
 */
static int find_SFD_and_shift_data(uint8_t *data, int *data_length,
		uint8_t sfd, int preamble_length)
{
	int i;
	int sfd_start_postion = preamble_length - 1;
	int data_start_position = sfd_start_postion + 1;
	int shift = 8;
	int no_shift, one_bit_shift, two_bit_shift;

	if (*data_length < sfd_start_postion + 2)
		return -EFAULT;

	no_shift = number_of_equal_bits(sfd, data[sfd_start_postion], 8);
	one_bit_shift = number_of_equal_bits(sfd,
				((data[sfd_start_postion] << 8) |
				data[sfd_start_postion+1]) >> 7, 8);
	two_bit_shift = number_of_equal_bits(sfd,
				((data[sfd_start_postion] << 8) |
				data[sfd_start_postion+1]) >> 6, 8);

	if(no_shift < 7 && one_bit_shift < 7 && two_bit_shift < 7) {
		PRINT_KRIT("SFD not found.");
		return 0;
	}

	if (one_bit_shift >= 7)
		shift -= 1;
	else if (two_bit_shift >= 7)
		shift -= 2;

	PRINT_KRIT("Data will be shifted by %d bits to the right", shift);

	for (i = 0; i < *data_length - sfd_start_postion - 1; ++i) {
		data[i] = ((data[i + data_start_position] << 8) |
				data[i + data_start_position + 1]) >> shift;
	}
	*data_length -= (sfd_start_postion + 1);

	return shift;
}

/**
 * Processes the raw data received from the chip and delegates the corrected
 * data to the IEEE 802.15.4 network stack.
 */
static int lprf_receive_ieee802154_data(struct lprf_local *lprf,
		uint8_t *buffer, int buffer_length)
{
	int frame_length = 0;
	struct sk_buff *skb;
	int ret = 0;
	int lqi = 0;

	if (find_SFD_and_shift_data(buffer, &buffer_length, 0xe5, 4) == 0) {
		PRINT_KRIT("SFD not found, ignoring frame");
		return -EINVAL;
	}

	frame_length = buffer[0];

	if (!ieee802154_is_valid_psdu_len(frame_length)) {
		dev_vdbg(&lprf->spi_device->dev, "corrupted frame received\n");
		frame_length = IEEE802154_MTU;
	}

	if (frame_length > buffer_length) {
		PRINT_KRIT("frame length greater than received data length");
		return -EINVAL;
	}
	PRINT_KRIT("Length of received frame is %d", frame_length);

	skb = dev_alloc_skb(frame_length);
	if (!skb) {
		dev_vdbg(&lprf->spi_device->dev,
				"failed to allocate sk_buff\n");
		return -ENOMEM;
	}

	memcpy(skb_put(skb, frame_length), buffer + 1, frame_length);
	ieee802154_rx_irqsafe(lprf->hw, skb, lqi);

	return ret;
}

/**
 * Reverses the bit order and inverts all bits in the data buffer.
 */
static void preprocess_received_data(uint8_t *data, int length)
{
	int i = 0;
	for (i = 0; i < length; ++i) {
		reverse_bit_order(&data[i]);
		data[i] = ~data[i];
	}
}

/**
 * Writes the received raw data to the char driver buffer, if a user space
 * application has actually opened the device file and is waiting for data.
 */
static void write_data_to_char_driver(uint8_t *data, int length)
{
	if (!atomic_read(&lprf_char_driver_interface.is_ready))
		return;

	kfifo_in(&lprf_char_driver_interface.data_buffer, data, length);
}

/**
 * Completion callback of the frame read command. Processes the received data
 * by calling lprf_receive_ieee802154_data(). The physical status information
 * will be polled from the chip, after the data was received successfully.
 */
static void __lprf_read_frame_complete(void *context)
{
	uint8_t *data_buf = 0;
	uint8_t phy_status = 0;
	int rc;
	int length = 0;
	struct lprf_local *lprf = context;
	struct lprf_state_change *state_change = &lprf->state_change;

	PRINT_KRIT("Frame read via SPI completed");

	data_buf = state_change->rx_buf + 2;

	phy_status = state_change->rx_buf[0];
	length = state_change->rx_buf[1];

	preprocess_received_data(data_buf, length);
	write_data_to_char_driver(data_buf, length);
	wake_up(&lprf_char_driver_interface.wait_for_rx_data);

	lprf_receive_ieee802154_data(lprf, data_buf, length);
	atomic_dec(&state_change->transition_in_progress);
	rc = lprf_phy_status_async(&lprf->phy_status);
	if (rc)
		PRINT_KRIT("PHY_STATUS BUSY in __lprf_read_frame_complete");

}

/**
 * Starts reading RX data from the chip. The chip should actually have data
 * available and be in sleep mode that no new data is received during the
 * read process.
 */
static void read_lprf_fifo(struct lprf_local *lprf)
{
	int ret = 0;
	struct lprf_state_change *state_change = &lprf->state_change;
	state_change->spi_message.complete = __lprf_read_frame_complete;
	state_change->spi_transfer.len = MAX_SPI_BUFFER_SIZE;

	memset(state_change->tx_buf, 0, sizeof(state_change->tx_buf));
	state_change->tx_buf[0] = FRMR;


	PRINT_KRIT("Will start async SPI read for frame read");

	ret = spi_async(lprf->spi_device, &state_change->spi_message);
	if (ret)
		lprf_async_error(lprf, state_change, ret);
}

/*
 * Resets some parts of the lprf chip
 *
 * @context: lprf_local struct containing the chip information
 *
 * After receiving RX data the internal statemachine of the lprf chip
 * does not reset all parts of the chip correctly. To avoid data
 * corruption and ensure a correct function of the chip some of those
 * resets need to be handled manually. This needs to be done every time
 * before the chip changes to RX mode or to TX mode.
 */
static void lprf_rx_resets(void *context)
{
	static int reset_counter = 0;
	struct lprf_local *lprf = context;
	struct lprf_state_change *state_change = &lprf->state_change;

	switch (reset_counter) {
	case 0:
		lprf_async_write_register(state_change, RG_SM_MAIN,
				0x05, lprf_rx_resets);
		reset_counter++;
		return;
	case 1:
		lprf_async_write_register(state_change, RG_SM_MAIN,
				0x0F, lprf_rx_resets);
		reset_counter++;
		return;
	case 2:
		lprf_async_write_subreg(state_change,
				state_change->dem_main_value,
				SR_DEM_RESETB, 0, lprf_rx_resets);
		reset_counter++;
		return;
	case 3:
		lprf_async_write_subreg(state_change,
				state_change->dem_main_value,
				SR_DEM_RESETB, 1, lprf_rx_resets);
		reset_counter++;
		return;
	case 4:
		if (state_change->to_state == STATE_CMD_TX) {
			lprf_start_frame_write(lprf);
			reset_counter = 0;
		}
		else {
			lprf_async_write_subreg(state_change,
					state_change->sm_main_value,
					SR_SM_COMMAND, STATE_CMD_RX,
					lprf_rx_resets);
			reset_counter++;
		}
		return;
	case 5:
		lprf_async_write_subreg(state_change,
				state_change->sm_main_value,
				SR_SM_COMMAND, STATE_CMD_NONE,
				lprf_rx_change_complete);
		reset_counter = 0;
		return;
	default:
		PRINT_DEBUG("Internal error in lprf_rx_resets");
	}
}

/**
 * Initiates a asynchronous state change
 *
 * @lprf: lprf_local struct containing chip information
 * @state: new state to change to
 */
static void lprf_async_state_change(struct lprf_local *lprf, uint8_t state)
{
	struct lprf_state_change *state_change = &lprf->state_change;
	state_change->to_state = state;

	switch (state) {
	case STATE_CMD_RX:
		PRINT_KRIT("Will change state to RX...");
		lprf_rx_resets(lprf);
		break;
	case STATE_CMD_TX:
		lprf_async_write_subreg(state_change,
				state_change->sm_main_value,
				SR_SM_COMMAND, STATE_CMD_SLEEP,
				lprf_rx_resets);
		PRINT_KRIT("Changed state to sleep, will change to TX");
		break;
	default:
		PRINT_DEBUG("Unsupported state change to state 0x%X", state);
	}
}

/**
 * Decides what action needs to be done dependent on the physical status of
 * the chip.
 *
 * @lprf: lprf_local struct containing chip information
 * @state_change: lprf_state_change struct
 * @phy_status: current physical status received from chip
 *
 * This is the main function for all state changes and other actions to be
 * performed. It gets the current physical status of the chip as an input
 * and decides what action will be performed. If the chip is busy or a
 * state transition is currently in progress this
 * function will do nothing. If the chip has RX data available an RX read
 * will be started. If there is pending TX data the chip will change to TX
 * mode.
 */
static void lprf_evaluate_phy_status(struct lprf_local *lprf,
		struct lprf_state_change *state_change, uint8_t phy_status)
{
	PRINT_KRIT("Phy_status in lprf_evaluate_phy_status 0x%X", phy_status);

	/* try lock following section. If already locked: return. */
	if (atomic_inc_return(&state_change->transition_in_progress) != 1) {
		atomic_dec(&state_change->transition_in_progress);
		PRINT_KRIT("transition in progress... abort");
		return;
	}

	/*
	 * Call ieee802154_xmit_complete() if tx transmission completed
	 * successfully.
	 */
	if(PHY_SM_STATUS(phy_status) != PHY_SM_SENDING &&
			state_change->tx_complete)
		lprf_tx_complete(lprf);

	/* Read data from chip, if RX data is available */
	if(PHY_SM_STATUS(phy_status) == PHY_SM_SLEEP &&
			!PHY_FIFO_EMPTY(phy_status)) {
		read_lprf_fifo(lprf);
		return;
	}

	/* Send TX data, if TX data is pending */
	if (lprf->tx_skb && PHY_FIFO_EMPTY(phy_status)) {
		lprf_async_state_change(lprf, STATE_CMD_TX);
		return;
	}

	/*
	 * Change to RX state again, if chip is in an idle state
	 * (RX data transferred to driver, chip still in sleep mode)
	 */
	if(PHY_SM_STATUS(phy_status) == PHY_SM_SLEEP &&
			PHY_FIFO_EMPTY(phy_status)) {
		lprf_async_state_change(lprf, STATE_CMD_RX);
		return;
	}

	/* unlock critical section */
	atomic_dec(&state_change->transition_in_progress);

	if(PHY_SM_STATUS(phy_status) == PHY_SM_RECEIVING) {
		if (PHY_FIFO_EMPTY(phy_status))
			lprf_start_polling_timer(lprf, RX_POLLING_INTERVAL);
		else
			lprf_start_polling_timer(lprf, RETRY_INTERVAL);
		return;
	}

	lprf_start_polling_timer(lprf, RETRY_INTERVAL);
}


/***
 *       ____        _  _  _                   _
 *      / ___| __ _ | || || |__    __ _   ___ | | __ ___
 *     | |    / _` || || || '_ \  / _` | / __|| |/ // __|
 *     | |___| (_| || || || |_) || (_| || (__ |   < \__ \
 *      \____|\__,_||_||_||_.__/  \__,_| \___||_|\_\|___/
 *
 * This section implements the callbacks of the IEEE 802.15.4 network statck.
 */

/**
 * Called when the WPAN device is activated from user space. Starts the
 * polling of the chip.
 */
static int lprf_start_ieee802154(struct ieee802154_hw *hw)
{
	struct lprf_local *lprf = hw->priv;

	PRINT_DEBUG("Call lprf_start_ieee802154...");

	atomic_set(&lprf->rx_polling_active, 1);
	lprf_phy_status_async(&lprf->phy_status);

	return 0;
}

/**
 * Called when the WPAN device gets deactivated from user space. Stops the
 * polling and changes to sleep mode.
 */
static void lprf_stop_ieee802154(struct ieee802154_hw *hw)
{
	struct lprf_local *lprf = hw->priv;
	lprf_stop_polling(lprf);

	/* Wait some time to make sure all pending communication finished*/
	usleep_range(900, 1000);

	lprf_write_subreg(lprf, SR_SM_COMMAND, STATE_CMD_SLEEP);
	lprf_write_subreg(lprf, SR_SM_COMMAND, STATE_CMD_NONE);
	lprf_write_subreg(lprf, SR_DEM_RESETB,  0);
	lprf_write_subreg(lprf, SR_DEM_RESETB,  1);
	lprf_write_subreg(lprf, SR_FIFO_RESETB, 0);
	lprf_write_subreg(lprf, SR_FIFO_RESETB, 1);
	lprf_write_subreg(lprf, SR_SM_RESETB,   0);
	lprf_write_subreg(lprf, SR_SM_RESETB,   1);
}

/**
 * callback for setting the RF channel. Sets the PLL values of the chip.
 */
static int
lprf_set_ieee802154_channel(struct ieee802154_hw *hw,u8 page, u8 channel)
{
	int pll_int = 0;
	int pll_frac = 0;
	int rf_freq = 0;
	int ret = 0;
	int vco_tune = 0;
	struct lprf_local *lprf = hw->priv;

	if (page != 0) {
		PRINT_DEBUG("Invalid channel page %d.", page);
		return -EINVAL;
	}

	rf_freq = calculate_rf_center_freq(channel);
	PRINT_DEBUG("RF-freq = %u", rf_freq);

	/* for RX */
	ret = lprf_calculate_pll_values(rf_freq, 1000000, &pll_int, &pll_frac);

	RETURN_ON_ERROR( lprf_write_subreg(lprf, SR_RX_CHAN_INT, pll_int) );
	RETURN_ON_ERROR( lprf_write_subreg(lprf,
			SR_RX_CHAN_FRAC_H, BIT24_H_BYTE(pll_frac)) );
	RETURN_ON_ERROR( lprf_write_subreg(lprf,
			SR_RX_CHAN_FRAC_M, BIT24_M_BYTE(pll_frac)) );
	RETURN_ON_ERROR( lprf_write_subreg(lprf,
			SR_RX_CHAN_FRAC_L, BIT24_L_BYTE(pll_frac)) );
	PRINT_DEBUG("Set RX PLL values to int=%d and frac=0x%.6x",
			pll_int, pll_frac);

	/* for TX */
	ret = lprf_calculate_pll_values(rf_freq, 0, &pll_int, &pll_frac);

	RETURN_ON_ERROR( lprf_write_subreg(lprf, SR_TX_CHAN_INT, pll_int) );
	RETURN_ON_ERROR( lprf_write_subreg(lprf,
			SR_TX_CHAN_FRAC_H, BIT24_H_BYTE(pll_frac)) );
	RETURN_ON_ERROR( lprf_write_subreg(lprf,
			SR_TX_CHAN_FRAC_M, BIT24_M_BYTE(pll_frac)) );
	RETURN_ON_ERROR( lprf_write_subreg(lprf,
			SR_TX_CHAN_FRAC_L, BIT24_L_BYTE(pll_frac)) );
	PRINT_DEBUG("Set TX PLL values to int=%d and frac=0x%.6x",
			pll_int, pll_frac);

	vco_tune = calc_vco_tune(channel);
	RETURN_ON_ERROR( lprf_write_subreg(lprf, SR_PLL_VCO_TUNE, vco_tune) );
	PRINT_DEBUG("Set VCO TUNE to %d", vco_tune);

	return ret;
}

/* TODO actually characterize power, values in 0.01dBm */
static const s32 lprf_tx_powers[] = {
		0, 100, 200, 300, 400, 500, 600, 700, 800, 900,
		1000, 1100, 1200, 1300, 1400, 1500
};

/**
 * Callback for setting the output power of the chip. Adjusts the
 * SR_TX_PWR_CTRL register. Note that the TX characteristics for the
 * settings used in this driver are not determined correctly. So the
 * output power will actually be the value set from user space. A higher
 * value will result in a higher output power.
 */
static int lprf_set_tx_power(struct ieee802154_hw *hw, s32 power)
{
	int i;
	struct lprf_local *lprf = hw->priv;

	for (i = 0; i < lprf->hw->phy->supported.tx_powers_size; i++) {
		if (lprf->hw->phy->supported.tx_powers[i] == power) {
			PRINT_DEBUG("Set SR_TX_PWR_CTRL to %d", i);
			return lprf_write_subreg(lprf, SR_TX_PWR_CTRL, i);
		}
	}

	return -EINVAL;
}

/**
 * Callback for available TX data. Initiates a phy_status poll get the
 * chip in TX mode and send the available data.
 */
static int
lprf_xmit_ieee802154_async(struct ieee802154_hw *hw, struct sk_buff *skb)
{
	int rc = 0;
	struct lprf_local *lprf = hw->priv;

	if (lprf->tx_skb) {
		PRINT_DEBUG("ERROR in xmit, buffer not empty yet");
		return -EBUSY;
	}

	lprf->tx_skb = skb;

	rc = lprf_phy_status_async(&lprf->phy_status);
	if (rc)
		PRINT_KRIT("PHY STATUS busy in lprf_xmit_ieee802154_async");

	PRINT_KRIT("Wrote %d bytes to TX buffer", skb->len);
	return 0;
}

/**
 * IEEE 802.15.4 uses CSMA-CA algorithms for channel access. Therefore
 * compatible chips need to be able to determine if the channel is currently
 * in use. The LPRF-chip does not support energy detection yet. As this
 * function needs to be implemented for the network stack to work, this
 * provides a dummy implementation for energy detection.
 */
static int lprf_ieee802154_energy_detection(struct ieee802154_hw *hw, u8 *level)
{
	PRINT_DEBUG("Called unsupported function "
			"lprf_ieee802154_energy_detection()");
	return 0;
}

/**
 * For the monitor interface to work the chip needs to support promiscuous
 * mode. This means that the chip only works in a kind of monitoring mode
 * with some IEEE specific features like address filtering turned off. As
 * our chip does not support any specific IEEE features on chip it basically
 * always works in promiscuous mode. Therefore this functions does not need
 * to do any specific communication with the chip.
 */
static int
lprf_set_promiscuous_mode(struct ieee802154_hw *hw, const bool on)
{
	PRINT_DEBUG("Set promiscuous mode");
	return 0;
}

static const struct ieee802154_ops  ieee802154_lprf_callbacks = {
	.owner = THIS_MODULE,
	.start = lprf_start_ieee802154,
	.stop = lprf_stop_ieee802154,
	.xmit_sync = 0, /* should not be used anymore */
	.xmit_async = lprf_xmit_ieee802154_async,
	.ed = lprf_ieee802154_energy_detection, /* not supported by hardware */
	.set_channel = lprf_set_ieee802154_channel,
	.set_hw_addr_filt = 0,
	.set_txpower = lprf_set_tx_power,
	.set_lbt = 0,		   /* Disabled in hw_flags */
	.set_cca_mode = 0,
	.set_cca_ed_level = 0,
	.set_csma_params = 0,	   /* Disabled in hw_flags */
	.set_frame_retries = 0,	   /* Disabled in hw_flags */
	.set_promiscuous_mode = lprf_set_promiscuous_mode,
};


/***
 *       ____  _                        _        _
 *      / ___|| |__    __ _  _ __    __| | _ __ (_)__   __ ___  _ __
 *     | |    | '_ \  / _` || '__|  / _` || '__|| |\ \ / // _ \| '__|
 *     | |___ | | | || (_| || |    | (_| || |   | | \ V /|  __/| |
 *      \____||_| |_| \__,_||_|     \__,_||_|   |_|  \_/  \___||_|
 *
 * This sections contains all functions related to the char driver interface.
 * This is only used as a debug interface and therefore not needed for a
 * proper IEEE 802.15.4 function.
 */

int lprf_open_char_device(struct inode *inode, struct file *filp)
{
	struct lprf_local *lprf = 0;
	int ret = 0;
	lprf = container_of(inode->i_cdev, struct lprf_local, my_char_dev);
	filp->private_data = lprf;

	if (atomic_inc_return(&lprf_char_driver_interface.is_open) != 1) {
		atomic_dec(&lprf_char_driver_interface.is_open);
		return -EMFILE;
	}

	ret = kfifo_alloc(&lprf_char_driver_interface.data_buffer,
			2024, GFP_KERNEL);
	if (ret)
		return ret;

	atomic_set(&lprf_char_driver_interface.is_ready, 1);
	PRINT_DEBUG("LPRF successfully opened as char device");
	return 0;


}

int lprf_release_char_device(struct inode *inode, struct file *filp)
{
	struct lprf_local *lprf;
	lprf = container_of(inode->i_cdev, struct lprf_local, my_char_dev);

	atomic_set(&lprf_char_driver_interface.is_ready, 0);

	kfifo_free(&lprf_char_driver_interface.data_buffer);
	atomic_dec(&lprf_char_driver_interface.is_open);

	PRINT_DEBUG("LPRF char device successfully released");
	return 0;
}

ssize_t lprf_read_char_device(struct file *filp,
		char __user *buf, size_t count, loff_t *f_pos)
{
	int buffer_length = 0;
	int bytes_to_copy = 0;
	int bytes_copied = 0;
	int ret = 0;

	PRINT_KRIT("Read from user space with buffer size %d requested", count);

	if( kfifo_is_empty(&lprf_char_driver_interface.data_buffer) ) {
		PRINT_KRIT("Read_char_device goes to sleep.");
		ret = wait_event_interruptible(
			lprf_char_driver_interface.wait_for_rx_data,
			!kfifo_is_empty(
				&lprf_char_driver_interface.data_buffer));
		if (ret < 0)
			return ret;
		PRINT_KRIT("Returned from sleep in read_char_device.");
	}

	buffer_length = kfifo_len(&lprf_char_driver_interface.data_buffer);
	bytes_to_copy = (count < buffer_length) ? count : buffer_length;

	ret = kfifo_to_user(&lprf_char_driver_interface.data_buffer,
			buf, bytes_to_copy, &bytes_copied);
	if(ret)
		return ret;

	PRINT_KRIT("%d/%d bytes copied to user.",
			bytes_copied, buffer_length);

	return bytes_copied;
}

ssize_t lprf_write_char_device(struct file *filp, const char __user *buf,
		size_t count, loff_t *f_pos)
{
	int bytes_copied = 0;
	int bytes_to_copy = 0;
	int ret = 0;
	struct sk_buff *skb;
	struct lprf_local *lprf = filp->private_data;

	PRINT_KRIT("Enter write char device");

	if (lprf->tx_skb) {
		PRINT_KRIT("Read_char_device goes to sleep.");
		ret = wait_event_interruptible(
				lprf_char_driver_interface.wait_for_tx_ready,
			!lprf->tx_skb);
	}

	bytes_to_copy = count < FRAME_LENGTH ? count : FRAME_LENGTH;
	skb = dev_alloc_skb(bytes_to_copy);
	if (!skb)
		return -ENOMEM;

	bytes_copied = bytes_to_copy - copy_from_user(
			skb_put(skb, bytes_to_copy), buf, bytes_to_copy);
	PRINT_KRIT("Copied %d/%d files to TX buffer", bytes_copied, count);

	skb->len = bytes_copied;
	lprf->tx_skb = skb;
	lprf->free_skb = true;

	PRINT_KRIT("Call state change from write char device");

	ret = lprf_phy_status_async(&lprf->phy_status);
	if (ret)
		PRINT_KRIT("phy status busy in lprf_write_char_device");

	PRINT_KRIT("Return from write char device");
	return bytes_copied;
}

/**
 * Defines the callback functions for file operations
 * when the lprf device is used with the char driver interface
 */
static const struct file_operations lprf_fops = {
	.owner =             THIS_MODULE,
	.read =              lprf_read_char_device,
	.write =             lprf_write_char_device,
	.unlocked_ioctl =    0, /* can be implemented for additional control */
	.open =              lprf_open_char_device,
	.release =           lprf_release_char_device,
};

/**
 * Registers the LPRF-Chip as char-device. Make sure to execute this function
 * only after the chip has been fully initialized and is ready to use.
 */
static int register_char_device(struct lprf_local *lprf)
{
	int ret = 0;
	dev_t dev_number = 0;

	ret = alloc_chrdev_region(&dev_number, 0, 1, "lprf");
	if (ret) {
		PRINT_DEBUG("Dynamic Device number allocation failed");
		return ret;
	}

	cdev_init(&lprf->my_char_dev, &lprf_fops);
	lprf->my_char_dev.owner = THIS_MODULE;
	ret = cdev_add (&lprf->my_char_dev, dev_number, 1);
	if (ret) {
		goto unregister;
	}
	PRINT_DEBUG("Successfully added char driver to system");
	return ret;

unregister:
	unregister_chrdev_region(dev_number, 1);
	return ret;
}

static inline void unregister_char_device(struct lprf_local *lprf)
{
	dev_t dev_number = lprf->my_char_dev.dev;
	cdev_del(&lprf->my_char_dev);
	unregister_chrdev_region(dev_number, 1);
	PRINT_DEBUG( "Removed Char Device");
}


/***
 *      ___         _  _
 *     |_ _| _ __  (_)| |_  ___
 *      | | | '_ \ | || __|/ __|
 *      | | | | | || || |_ \__ \
 *     |___||_| |_||_| \__||___/
 *
 * This section contains all kinds of initializations only during module
 * loading. The starting point of this module is the lprf_probe()
 * function, which calls all other initialization functions.
 */

/**
 * initializes the lprf chip with the default configuration.
 */
static int init_lprf_hardware(struct lprf_local *lprf)
{
	int ret = 0;
	unsigned int value = 0;
	int rx_counter_length =
			get_rx_length_counter(KBIT_RATE, FRAME_LENGTH);

	/* Reset all and load initial values */
	RETURN_ON_ERROR(__lprf_write(lprf, RG_GLOBAL_RESETB,  0x00));
	RETURN_ON_ERROR(__lprf_write(lprf, RG_GLOBAL_RESETB,  0xFF));
	RETURN_ON_ERROR(__lprf_write(lprf, RG_GLOBAL_initALL, 0xFF));

	/* Clock Reference */
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_CTRL_CLK_CDE_OSC, 0));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_CTRL_CLK_CDE_PAD, 1));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_CTRL_CLK_DIG_OSC, 0));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_CTRL_CLK_DIG_PAD, 1));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_CTRL_CLK_PLL_OSC, 0));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_CTRL_CLK_PLL_PAD, 1));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_CTRL_CLK_C3X_OSC, 0));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_CTRL_CLK_C3X_PAD, 1));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_CTRL_CLK_FALLB,   0));

	/* ADC_CLK */
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_CTRL_CDE_ENABLE, 0));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_CTRL_C3X_ENABLE, 1));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_CTRL_CLK_ADC,    1));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_CTRL_C3X_LTUNE,  1));

	/* LDOs */
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_LDO_A_VOUT,     21));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_LDO_D_VOUT,     24));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_LDO_PLL_VOUT,   24));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_LDO_VCO_VOUT,   24));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_LDO_TX24_VOUT,  23));

	/* PLL Configuration */
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_IREF_PLL_CTRLB,   0));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_PLL_VCO_TUNE,   235));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_PLL_LPF_C,        0));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_PLL_LPF_R,        9));

	/* activate 2.4GHz Band */
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_RX_RF_MODE,     0));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_RX_LO_EXT,      0));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_LNA24_ISETT,    7));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_LNA24_SPCTRIM, 15));

	/* ADC Settings */
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_CTRL_ADC_MULTIBIT, 0));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_CTRL_ADC_ENABLE,   1));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_CTRL_ADC_BW_SEL,   1));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_CTRL_ADC_BW_TUNE,  5));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_CTRL_ADC_DR_SEL,   2));

	/* Polyphase Filter Setting */
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_PPF_M0,    0));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_PPF_M1,    0));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_PPF_TRIM,  0));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_PPF_HGAIN, 1));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_PPF_LLIF,  0));

	/* Demodulator Settings */
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_DEM_CLK96_SEL,          1));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_DEM_AGC_EN,             1));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_DEM_FREQ_OFFSET_CAL_EN, 0));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_DEM_OSR_SEL,            0));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_DEM_BTLE_MODE,          1));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_DEM_IF_SEL,             2));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_DEM_DATA_RATE_SEL,      3));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_DEM_IQ_CROSS,           1));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_DEM_IQ_INV,             0));

	/* initial CIC Filter gain settings */
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_DEM_GC1, 0));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_DEM_GC2, 0));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_DEM_GC3, 1));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_DEM_GC4, 0));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_DEM_GC5, 0));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_DEM_GC6, 1));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_DEM_GC7, 4));

	/* General TX Settings */
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_PLL_MOD_DATA_RATE,   3));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_PLL_MOD_FREQ_DEV,   21));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_TX_EN,               1));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_TX_ON_CHIP_MOD,      1));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_TX_UPS,              0));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_TX_ON_CHIP_MOD_SP,   0));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_TX_AMPLI_OUT_MAN_H,  1));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_TX_AMPLI_OUT_MAN_L, 255));


	/* STATE MASCHINE CONFIGURATION */

	/* General state machine settings */
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_FIFO_MODE_EN,    1));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_WAKEUPONSPI,     1));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_WAKEUPONRX,      0));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_WAKEUP_MODES_EN, 0));

	/* Startup counter Settings */
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_SM_TIME_POWER_TX, 0xff));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_SM_TIME_POWER_RX, 0xff));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_SM_TIME_PLL_PON,  0xff));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_SM_TIME_PLL_SET,  0xff));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_SM_TIME_TX,       0xff));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_SM_TIME_PD_EN,    0xff));

	/* SM TX */
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_TX_MODE,          0));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_INVERT_FIFO_CLK,  0));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_DIRECT_RX,        1));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_TX_ON_FIFO_IDLE,  0));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_TX_ON_FIFO_SLEEP, 0));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_TX_IDLE_MODE_EN,  0));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_TX_PWR_CTRL,     15));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_TX_MAXAMP,        0));

	/* SM RX */
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_DIRECT_TX,          0));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_DIRECT_TX_IDLE,     0));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_RX_HOLD_MODE_EN,    0));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_RX_TIMEOUT_EN,      0));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_RX_HOLD_ON_TIMEOUT, 0));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_AGC_AUTO_GAIN,      0));

	/* Package counter */
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_RX_LENGTH_H,
			BIT24_H_BYTE(rx_counter_length)));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_RX_LENGTH_M,
			BIT24_M_BYTE(rx_counter_length)));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_RX_LENGTH_L,
			BIT24_L_BYTE(rx_counter_length)));

	/* Timeout counter */
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_RX_TIMEOUT_H, 0xFF));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_RX_TIMEOUT_M, 0xFF));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_RX_TIMEOUT_L, 0xFF));

	/* Resets */
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_FIFO_RESETB, 0));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_FIFO_RESETB, 1));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_SM_EN,       1));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_SM_RESETB,   0));
	RETURN_ON_ERROR(lprf_write_subreg(lprf, SR_SM_RESETB,   1));

	/* Save configuration of SM_MAIN and DEM_MAIN for async SPI transfers */
	__lprf_read(lprf, RG_SM_MAIN, &value);
	lprf->state_change.sm_main_value = value & 0x0f;
	__lprf_read(lprf, RG_DEM_MAIN, &value);
	lprf->state_change.dem_main_value = value;

	/* Set PLL to correct RF channel */
	lprf_set_ieee802154_channel(lprf->hw,
			lprf->hw->phy->current_page,
			lprf->hw->phy->current_channel);

	return 0;
}

/**
 * Reads the chip ID and sets the IEEE device capabilities
 * for this chip.
 *
 * Returns zero or a negative error code.
 */
static int lprf_detect_device(struct lprf_local *lprf)
{
	int rx_buf = 0, ret=0, chip_id = 0;

	RETURN_ON_ERROR( __lprf_read(lprf, RG_CHIP_ID_H, &rx_buf) );
	chip_id |= (rx_buf << 8);

	RETURN_ON_ERROR( __lprf_read(lprf, RG_CHIP_ID_L, &rx_buf) );
	chip_id |= rx_buf;

	if (chip_id != 0x1a51) {
		dev_err(&lprf->spi_device->dev, "Device with invalid "
				"Chip ID %X found", chip_id);
		return -ENODEV;
	}


	lprf->hw->flags = IEEE802154_HW_PROMISCUOUS |
			IEEE802154_HW_RX_DROP_BAD_CKSUM;

	lprf->hw->phy->flags = WPAN_PHY_FLAG_TXPOWER;

	lprf->hw->phy->supported.cca_modes = 0;
	lprf->hw->phy->supported.cca_opts = 0;
	lprf->hw->phy->supported.cca_ed_levels = 0;
	lprf->hw->phy->supported.cca_ed_levels_size = 0;
	lprf->hw->phy->cca.mode = NL802154_CCA_ENERGY;

	lprf->hw->phy->supported.channels[0] = 0x7FFF800;
	lprf->hw->phy->current_channel = 11;
	lprf->hw->phy->symbol_duration = 16;
	lprf->hw->phy->supported.tx_powers = lprf_tx_powers;
	lprf->hw->phy->supported.tx_powers_size =
			ARRAY_SIZE(lprf_tx_powers);

	lprf->hw->phy->cca_ed_level = 42;
	lprf->hw->phy->transmit_power = 15;

	dev_info(&lprf->spi_device->dev,"LPRF Chip found with Chip ID %X",
			chip_id);
	return 0;

}

/**
 * Initializes the lprf_local struct
 */
static void init_lprf_local(struct lprf_local *lprf, struct spi_device *spi)
{
	hrtimer_init(&lprf->rx_polling_timer, CLOCK_MONOTONIC,
			HRTIMER_MODE_REL);
	lprf->rx_polling_timer.function = lprf_start_poll;

	lprf->spi_device = spi;
	spi_set_drvdata(spi, lprf);
}

/**
 * Initializes the lprf_state_change struct
 */
static void init_state_change(struct lprf_state_change *state_change,
		struct lprf_local *lprf, struct spi_device *spi)
{
	state_change->lprf = lprf;
	spi_message_init(&state_change->spi_message);
	state_change->spi_message.context = lprf;
	state_change->spi_message.spi = spi;
	state_change->spi_transfer.len = 3;
	state_change->spi_transfer.tx_buf = state_change->tx_buf;
	state_change->spi_transfer.rx_buf = state_change->rx_buf;
	spi_message_add_tail(&state_change->spi_transfer,
			&state_change->spi_message);
}

/**
 * Initializes the lprf_phy_status struct
 */
static void init_phy_status(struct lprf_phy_status *phy_status,
		struct lprf_local *lprf, struct spi_device *spi)
{
	phy_status->spi_device = spi;
	spi_message_init(&phy_status->spi_message);
	phy_status->spi_message.context = lprf;
	phy_status->spi_message.spi = spi;
	phy_status->spi_transfer.len = 1;
	phy_status->spi_transfer.tx_buf = phy_status->tx_buf;
	phy_status->spi_transfer.rx_buf = phy_status->rx_buf;
	spi_message_add_tail(&phy_status->spi_transfer,
			&phy_status->spi_message);
}

/**
 * Initializes the char_driver struct
 */
static void init_char_driver(void)
{
	init_waitqueue_head(&lprf_char_driver_interface.wait_for_rx_data);
	init_waitqueue_head(&lprf_char_driver_interface.wait_for_tx_ready);
}

/**
 * Starting point of the kernel module. Sets everything up correctly.
 */
static int lprf_probe(struct spi_device *spi)
{
	int ret = 0;
	struct lprf_local *lprf = 0;
	struct lprf_state_change *state_change = 0;
	struct lprf_phy_status *phy_status = 0;
	struct ieee802154_hw *hw = 0;
	PRINT_DEBUG( "Call lprf_probe");

	hw = ieee802154_alloc_hw(sizeof(*lprf),
			&ieee802154_lprf_callbacks);
	if( hw == 0)
		return -ENOMEM;
	PRINT_DEBUG("Successfully allocated ieee802154_hw structure");

	lprf = hw->priv;
	lprf->hw = hw;
	state_change = &lprf->state_change;
	phy_status = &lprf->phy_status;

	/* Init structs */
	init_lprf_local(lprf, spi);
	init_phy_status(phy_status, lprf, spi);
	init_state_change(state_change, lprf, spi);
	init_char_driver();

	hw->parent = &lprf->spi_device->dev;
	ieee802154_random_extended_addr(&hw->phy->perm_extended_addr);

	lprf->regmap = devm_regmap_init_spi(spi, &lprf_regmap_spi_config);
	if (IS_ERR(lprf->regmap)) {
		dev_err(&spi->dev, "Failed to allocate register map: %d",
				(int) PTR_ERR(lprf->regmap));
	}

	ret = lprf_detect_device(lprf);
	if(ret)
		goto free_lprf;

	ret = init_lprf_hardware(lprf);
	if(ret)
		goto free_lprf;
	PRINT_DEBUG("Hardware successfully initialized");

	ret = register_char_device(lprf);
	if(ret)
		goto free_lprf;

	ret = ieee802154_register_hw(hw);
	if (ret)
		goto unregister_char_device;
	PRINT_DEBUG("Successfully registered IEEE 802.15.4 device");

	return ret;

unregister_char_device:
	unregister_char_device(lprf);
free_lprf:
	ieee802154_free_hw(hw);

	return ret;
}

/**
 * Called when the module is unloaded. Frees storage and cleans up everything.
 */
static int lprf_remove(struct spi_device *spi)
{
	struct lprf_local *lprf = spi_get_drvdata(spi);

	lprf_stop_ieee802154(lprf->hw);
	unregister_char_device(lprf);

	ieee802154_unregister_hw(lprf->hw);
	ieee802154_free_hw(lprf->hw);
	dev_dbg(&spi->dev, "unregistered LPRF chip\n");

	return 0;
}

static const struct of_device_id lprf_of_match[] = {
	{ .compatible = "ias,lprf", },
	{ },
};
MODULE_DEVICE_TABLE(of, lprf_of_match);

static const struct spi_device_id lprf_device_id[] = {
	{ .name = "lprf", },
	{ },
};
MODULE_DEVICE_TABLE(spi, lprf_device_id);

static struct spi_driver lprf_driver = {
	.id_table = lprf_device_id,
	.driver = {
		.of_match_table = of_match_ptr(lprf_of_match),
		.name	= "lprf",
	},
	.probe      = lprf_probe,
	.remove     = lprf_remove,
};

module_spi_driver(lprf_driver);

MODULE_DESCRIPTION("LPRF Linux Device Driver for IEEE 802.15.4 network stack");
MODULE_LICENSE("GPL v2");



