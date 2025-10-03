Morse Micro IoT Software Development Kit for ESP-IDF (Alpha Port)
=================================================================

# Overview

This alpha port includes drivers and sample applications to add Morse Micro Wi-Fi HaLow to the
[ESP-IDF](https://idf.espressif.com/). The components are ported from the
[MM-IoT-SDK](https://www.github.com/MorseMicro/mm-iot-sdk)

## What is an Alpha Port
Morse Micro will provide Alpha ports of its software for some platforms. These ports are not part of
the standard test and development cycle for a software release and may be incomplete in the set of
supported features. They intend to provide a starting point for integrating Morse Micro software to
projects based on these platforms.

# How to use

## Follow the ESP-IDF Getting Started Guide

Before using these components please ensure you have followed the [ESP-IDF Getting Started Guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/index.html).

## Setup environment

Assuming you have followed the above guide you should have the required environment variables for
ESP-IDF. You can validate this by running the following. If this command fails to run please refer
back to the [ESP-IDF Getting Started
Guide](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/index.html).

> Note, when targeting the ESP32-C6, you must use ESP-IDF v5.2.2 or later.

```bash
idf.py --version
ESP-IDF v5.1.1
```

You need will need to clone this repo [mm-iot-esp32](https://github.com/MorseMicro/mm-iot-esp32)
repository and set the `MMIOT_ROOT` environment variable. If you cloned the repository into your
home directory, you can do this with the following command:

```bash
export MMIOT_ROOT=~/mm-iot-esp32
```

## Component Configuration

Some of the components delivered as part of the MM-IoT-SDK may require some configuration. For this
the [Kconfig
system](https://docs.espressif.com/projects/esp-idf/en/v5.1.1/esp32s3/api-reference/kconfig.html)
supplied as part of the ESP-IDF. To access this run the following command in your project.

```bash
idf.py menuconfig
```

### Morse Micro Shim Configuration
The configuration options are located in `(Top) → Component config → Morse Micro Shim Configuration`

This components allows you to configure:
- The pins used to communicate with the Morse Micro chip
- Board Configuration File (BCF) to link against

#### Board Configuration File
The Board Configuration File (BCF) is a configuration file for the Morse Micro chip that provides
board-specific information and is required for the system to function properly. BCFs for Morse Micro
modules are included in the MM-IoT-SDK package. These modules can be identified by the part numbers
printed on their labels, which take the form MM6108-MFxxxxxx.


## Build and Run Porting Assistant Test Application

This application is intended to validate the connections between the ESP32 chip and the Morse Micro
chip.

```bash
cd examples/porting_assistant
idf.py build
idf.py flash monitor
```

You will get a series of PASS/FAIL results logged out the UART. If you are getting any fail results
please review the Kconfig values used are correct for your board.

```
MM-IoT-SDK Porting Assistant
----------------------------

Memory allocation                                            [ PASS ]
Memory reallocation                                          [ PASS ]
Passage of time                                              [ PASS ]
Task creation and preemption                                 [ PASS ]
WLAN HAL initialisation                                      I (513) gpio: GPIO[3]| InputEn: 0| OutputEn: 1| OpenDrain: 0| Pullup: 0| Pulldown: 0| Intr:0
I (523) gpio: GPIO[8]| InputEn: 0| OutputEn: 1| OpenDrain: 0| Pullup: 0| Pulldown: 0| Intr:0
I (533) gpio: GPIO[10]| InputEn: 0| OutputEn: 1| OpenDrain: 0| Pullup: 0| Pulldown: 0| Intr:0
I (543) gpio: GPIO[9]| InputEn: 1| OutputEn: 0| OpenDrain: 0| Pullup: 0| Pulldown: 1| Intr:1
I (543) gpio: GPIO[21]| InputEn: 1| OutputEn: 0| OpenDrain: 0| Pullup: 0| Pulldown: 0| Intr:2

Hard reset device
Send training sequence
Check for MM chip ready for SDIO/SPI commands                [ PASS ]
Put MM chip into SPI mode                                    [ PASS ]
Validate MM firmware                                         [ PASS ]
Validate BCF                                                 [ PASS ]


11 total test steps. 8 passed, 0 failed, 3 no result, 0 skipped
I (803) main_task: Returned from app_main()
```

## Build and Run Scan Example Application

The scan example performs a scan for access points (APs) and displays the results. It is necessary
to make a small source code modification to set the country code to that of the applicable
regulatory domain. Edit the source code in `main/src/scan.c` and uncomment the following line,
changing AU to the appropriate two character country code:

```C
// #define COUNTRY_CODE "AU"
```

Building and flashing the application.

```bash
cd examples/scan
idf.py build
idf.py flash monitor
```

Output of this application will vary depending on the APs in range but will look something like the
following.

```
Morse Scan Demo (Built Aug  9 2023 17:39:12)

I (445) gpio: GPIO[3]| InputEn: 0| OutputEn: 1| OpenDrain: 0| Pullup: 0| Pulldown: 0| Intr:0
I (455) gpio: GPIO[8]| InputEn: 0| OutputEn: 1| OpenDrain: 0| Pullup: 0| Pulldown: 0| Intr:0
I (465) gpio: GPIO[10]| InputEn: 0| OutputEn: 1| OpenDrain: 0| Pullup: 0| Pulldown: 0| Intr:0
I (475) gpio: GPIO[9]| InputEn: 1| OutputEn: 0| OpenDrain: 0| Pullup: 0| Pulldown: 1| Intr:1
I (485) gpio: GPIO[21]| InputEn: 1| OutputEn: 0| OpenDrain: 0| Pullup: 0| Pulldown: 0| Intr:2
Morse firmware version rel_1_9_1_2023_Aug_07, morselib version bccf2e03c, Morse chip ID 0x206

Scan started on AU channels, Waiting for results...
I (1945) main_task: Returned from app_main()
 1. RSSI: -32, BSSID: 0c:bf:74:8b:72:62, Beacon Interval(TUs):   100, Capability Info: 0x0011, SSID: MorseMicro
Scanning completed.
```

## STA Connect example

The STA Connect example demonstrates how to connect as a station (STA) to an AP. For this example to
operate it requires that an AP be available to connect to and that the SSID and passphrase of the AP
match the defines in the source file `main/src/sta_connect.c`.

This example application will first establish connection then will send an ARP request for IP
address `192.168.1.1`. It will also print information about any received packets. It is necessary to
make a small source code modification to set the country code to that of the applicable regulatory
domain. Edit the source code in `main/src/sta_connect.c` and uncomment the following line, changing
AU to the appropriate two character country code:

```C
// #define COUNTRY_CODE "AU"
```

Building and flashing the application.

```bash
cd examples/scan
idf.py build
idf.py flash monitor
```

Output will vary depending on the AP configuration, but should look something like the following...

```
Morse STA Demo (Built Aug  9 2023 17:42:35)

I (446) gpio: GPIO[3]| InputEn: 0| OutputEn: 1| OpenDrain: 0| Pullup: 0| Pulldown: 0| Intr:0
I (456) gpio: GPIO[8]| InputEn: 0| OutputEn: 1| OpenDrain: 0| Pullup: 0| Pulldown: 0| Intr:0
I (466) gpio: GPIO[10]| InputEn: 0| OutputEn: 1| OpenDrain: 0| Pullup: 0| Pulldown: 0| Intr:0
I (476) gpio: GPIO[9]| InputEn: 1| OutputEn: 0| OpenDrain: 0| Pullup: 0| Pulldown: 1| Intr:1
I (486) gpio: GPIO[21]| InputEn: 1| OutputEn: 0| OpenDrain: 0| Pullup: 0| Pulldown: 0| Intr:2
Morse firmware version rel_1_9_1_2023_Aug_07, morselib version bccf2e03c, Morse chip ID 0x206

STA state: CONNECTING (1)
Link went Up
STA state: CONNECTED (2)
I (7476) main_task: Returned from app_main()
RX from 0c:bf:74:8b:72:62 type 0x0806
```

# License

This repository and its contents, including software component Makefiles and metadata, are licensed under the Apache 2.0, unless otherwise stated in individual files.

Software components from third party sources provide their own licenses.
