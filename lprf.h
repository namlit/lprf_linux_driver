/*
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
 * Written by:
 * Tilman Sinning <tilman.sinning@rwth-aachen.de
 * Moritz Schrey <mschrey@ias.rwth-aachen.de>
 * Dmitry Eremin-Solenikov <dbaryshkov@gmail.com>
 * Alexander Smirnov <alex.bluesman.smirnov@gmail.com>
 * Alexander Aring <aar@pengutronix.de>
 * Alessandro Rubini
 * Jonathan Corbet
 */

#ifndef _LPRF_H_
#define _LPRF_H_

// Remove comment to show debug outputs
#define LPRF_DEBUG

/*
 * Debug macro for timing critical parts like polling. Activating
 * this macro will lead to a lot of debug messages and should only
 * be used with decreased time resolution for polling (RX_POLLING_INTERVAL)
 */
// #define LPRF_DEBUG_KRIT

#define LPRF_MAX_BUF 256
#define FRAME_LENGTH 100 // select one byte more to take shifting into account
#define KBIT_RATE 2000
#define FIFO_PACKET_SIZE 256
#define MAX_SPI_BUFFER_SIZE (FIFO_PACKET_SIZE + 2)

#define RX_POLLING_INTERVAL ktime_set(0, 5000000)
#define RX_RX_INTERVAL ktime_set(0, 500000)
#define TX_RX_INTERVAL ktime_set(0, 5000000)

#define PHY_SM_STATUS(phy_status)   (((phy_status) & 0xe0) >> 5)
#define PHY_SM_ENABLE(phy_status)   (((phy_status) & 0x10) >> 4)
#define PHY_FIFO_EMPTY(phy_status)  (((phy_status) & 0x08) >> 3)
#define PHY_FIFO_FULL(phy_status)   (((phy_status) & 0x04) >> 2)

#define PHY_SM_DEEPSLEEP            0x01
#define PHY_SM_SLEEP                0x02
#define PHY_SM_BUSY                 0x03
#define PHY_SM_TX_RDY               0x04
#define PHY_SM_SENDING              0x05
#define PHY_SM_RX_RDY               0x06
#define PHY_SM_RECEIVING            0x07

#define COUNTER_H_BYTE(c) (((c) & 0xFF0000) >> 16)
#define COUNTER_M_BYTE(c) (((c) & 0x00FF00) >> 8)
#define COUNTER_L_BYTE(c) ((c) & 0x0000FF)

#undef PRINT_DEBUG
#ifdef LPRF_DEBUG
	#define PRINT_DEBUG(fmt, args...) printk( KERN_DEBUG "lprf: " fmt "\n", ## args)
#else
	#define PRINT_DEBUG(fmt, args...)
#endif

#undef PRINT_KRIT
#ifdef LPRF_DEBUG_KRIT
	#define PRINT_KRIT(fmt, args...) printk( KERN_DEBUG "lprf: " fmt "\n", ## args)
#else
	#define PRINT_KRIT(fmt, args...)
#endif


#define HANDLE_SPI_ERROR(f) ret = (f); \
	if (ret) { \
		mutex_unlock(&lprf->spi_mutex); \
		return ret; \
	}


/**
 * @ spi_message: And spi_message struct that can be used for asynchronous
 * 	spi transfers. Make sure to lock spi_mutex and set the correct callback
 * 	when using spi_message.
 */
struct lprf {
	struct spi_device *spi_device;
	struct regmap *regmap;
	struct mutex spi_mutex;
	struct cdev my_char_dev;
	struct spi_message spi_message;
	struct spi_transfer spi_transfer;
	uint8_t spi_rx_buf[MAX_SPI_BUFFER_SIZE];
	uint8_t spi_tx_buf[MAX_SPI_BUFFER_SIZE];
	struct hrtimer rx_polling_timer;
	DECLARE_KFIFO_PTR(rx_buffer, uint8_t);
	DECLARE_KFIFO_PTR(tx_buffer, uint8_t);
	struct ieee802154_hw *ieee802154_hw;
	struct work_struct poll_rx;
	atomic_t rx_polling_active;
	wait_queue_head_t wait_for_frmw_complete;
};


#endif // _LPRF_H_
