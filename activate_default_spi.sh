#!/bin/bash

LPRF_DIR=$(dirname "$0")

pushd ${LPRF_DIR} > /dev/null

if ifconfig | grep "wpan0" &> /dev/null
then
	echo "Deactivate WPAN interface..."
	ip link set wpan0 down
fi

if ifconfig | grep "monitor0" &> /dev/null
then
	echo "Deactivate monitor0 interface..."
	ip link set monitor0 down
fi

if lsmod | grep "lprf" &> /dev/null
then
	echo "Remove lprf kernel module from kernel..."
	rmmod lprf
else
	echo "LPRF module is not loaded."
fi

if dtoverlay -l | grep "lprf" &> /dev/null
then
	echo "Remove LPRF device tree overlay..."
	dtoverlay -r lprf
else
    echo "LPRF device tree overlay not loaded."
fi

if ls | grep "spi.dtbo" &> /dev/null && [ spi-overlay.dts -ot spi.dtbo ]
then
	echo "Device tree file binary already up to date."
else
	echo "Compile spi device tree file..."
	dtc -@ -I dts -O dtb -o spi.dtbo spi-overlay.dts
	
	if dtoverlay -l | grep "spi" &> /dev/null
	then
		echo "Remove spi from device tree..."
		dtoverlay -r spi
	fi
fi

if dtoverlay -l | grep "spi" &> /dev/null
then
	echo "Spi is already in device tree."
else
	echo "Load spi device tree into kernel..."
	dtoverlay spi.dtbo
fi

popd > /dev/null
