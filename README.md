# PLC WiFi Firmware on NodeMCU

Firmware for an ESP8266 NodeMCU that provides:

- WiFi provisioning through a captive portal
- Persistent WiFi credential storage in EEPROM
- A Modbus TCP server on port 502
- Basic PLC-style I/O memory areas (coils, discrete inputs, holding registers, input registers)
- Relay output control from Modbus coil writes

## Project Overview

On boot, the device tries to connect to previously saved WiFi credentials.

- If credentials are valid and connection succeeds, it starts a Modbus TCP server.
- If not, it starts an access point and captive portal so you can enter WiFi credentials.

After provisioning, credentials are saved and the module reboots automatically.

## Hardware and Pin Mapping

Relay output pins are configured as:

- D1 (GPIO5)
- D2 (GPIO4)
- D5 (GPIO14)
- D6 (GPIO12)

At startup, coil 0 is set ON by default, so the first relay is energized unless you change this behavior in code.

## WiFi Provisioning Behavior

When no saved WiFi config exists (or connection fails), the firmware enters provisioning mode:

- AP name: PLC-Setup-<chip_id_hex>
- AP password: plcsetup123
- Captive portal endpoint: / (root)
- Save endpoint: /save (HTTP POST)

The portal asks for SSID and password, stores them in EEPROM, and reboots.

## Modbus TCP Details

- Transport: Modbus TCP
- Port: 502
- Memory sizes:
  - Coils: 32
  - Discrete inputs: 32
  - Holding registers: 32
  - Input registers: 32

### Supported Function Codes

- 1: Read Coils
- 2: Read Discrete Inputs
- 3: Read Holding Registers
- 4: Read Input Registers
- 5: Write Single Coil
- 6: Write Single Holding Register
- 15: Write Multiple Coils
- 16: Write Multiple Holding Registers

### Data Behavior

- Coils are used as writable boolean outputs.
- Discrete inputs mirror the coil states.
- Holding registers are read/write words.
- Input registers include runtime values:
  - Input register 0: free heap (lower 16 bits)
  - Input register 1: RSSI shifted by +32768

## Build and Flash

Use Arduino IDE or PlatformIO with an ESP8266 board target.

Recommended Arduino IDE setup:

1. Board package: ESP8266 by ESP8266 Community
2. Board: NodeMCU 1.0 (ESP-12E Module)
3. Upload speed and flash settings as required by your module

Required libraries used by the sketch:

- ESP8266WiFi
- ESP8266WebServer
- DNSServer
- EEPROM

(These are typically available with the ESP8266 board package.)

## Typical First-Time Setup

1. Flash the firmware to your NodeMCU.
2. Open Serial Monitor at 115200 baud.
3. Connect your phone/laptop to AP PLC-Setup-<chip_id_hex> with password plcsetup123.
4. Open any website; captive portal should redirect to setup page.
5. Enter your WiFi SSID/password and submit.
6. Device reboots, joins WiFi, and starts Modbus TCP on port 502.
7. Use your PLC/SCADA/HMI Modbus TCP client to read/write addresses.

## Notes and Recommendations

- The provisioning AP password is hardcoded; change it if this is used in production.
- Modbus TCP is unauthenticated in this implementation; isolate the device network when possible.
- If WiFi drops during operation, firmware returns to provisioning mode.

## File Layout

- plc_wifi_firmware_on_NodeMCU.ino: Main firmware sketch
