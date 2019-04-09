PROGRAM = dimmer

EXTRA_COMPONENTS = \
	extras/http-parser \
	extras/dhcpserver \
	extras/rboot-ota \
	$(abspath ../../components/esp-8266/wifi_config) \
	$(abspath ../../components/common/wolfssl) \
	$(abspath ../../components/common/cJSON) \
	$(abspath ../../components/common/homekit)

FLASH_SIZE ?= 8
FLASH_MODE ?= dout
FLASH_SPEED ?= 40

# not sure if this requires modification for LCM/OTA?
HOMEKIT_SPI_FLASH_BASE_ADDR ?= 0x8c000

EXTRA_CFLAGS += -I../..

EXTRA_CFLAGS += -DWIFI_CONFIG_CONNECT_TIMEOUT=180000
EXTRA_CFLAGS += -DHOMEKIT_SHORT_APPLE_UUIDS
EXTRA_CFLAGS += -DHOMEKIT_OVERCLOCK_PAIR_VERIFY
EXTRA_CFLAGS += -DHOMEKIT_OVERCLOCK_PAIR_SETUP
EXTRA_CFLAGS += -DHOMEKIT_DEBUG=1

EXTRA_CFLAGS += -DDIMMER

include $(SDK_PATH)/common.mk

#monitor:
#	$(FILTEROUTPUT) --port $(ESPPORT) --baud 115200 --elf $(PROGRAM_OUT)

# standard output is redirected as UDP packets over WiFi instead of over UART
monitor:
	sudo tcpdump -i enp3s0 udp port 45678 -vv -X
