DEFINES+=PROJECT_CONF_H=\"project-conf.h\"	
PROJECT?=example

all: $(PROJECT)

CONTIKI_WITH_IPV6 = 1

APPS += mqtt-service-contiki-3.0

CONTIKI = ../..
include $(CONTIKI)/Makefile.include

flash:
	sudo /home/yuri/ti/uniflash_3.4/uniflash.sh -ccxml /home/yuri/ti/uniflash_3.4/CC2538SF53.ccxml -operatiDon Erase -program example.elf

init:	
	modprobe ftdi_sio vendor=0x403 product=0xa6d1
	modprobe ftdi_sio
	echo 0403 a6d1 > /sys/bus/usb-serial/drivers/ftdi_sio/new_id
	
