# ESPHome NUT server

Turn an ESP32 into a network UPS monitor. Plug your UPS's USB cable into the ESP32, and any
[NUT](https://networkupstools.org/) client on your network can see the UPS status and shut down
cleanly when the power goes out:

- Synology DSM, QNAP, TrueNAS, Unraid
- Linux and macOS `upsmon`
- Home Assistant's NUT integration

**Home Assistant is not required.** The ESP32 itself is the NUT server, so your NAS keeps getting
UPS status even if Home Assistant is down, or runs on the NAS that is shutting down.

## What you need

- **An ESP32 with USB host support: ESP32-S3 (recommended), ESP32-S2 or ESP32-P4.**
  The original ESP32 (boards labelled "ESP32-WROOM-32", `esp32dev`) has no USB host hardware and
  can't talk to a UPS. If you pick one, ESPHome stops with an error explaining this.
  A common choice is the ESP32-S3-DevKitC-1 (`board: esp32-s3-devkitc-1`).
- **A UPS with a USB port** that works with NUT's `usbhid-ups` driver. That covers most APC,
  CyberPower, Eaton, Tripp Lite and similar consumer UPSes. No model-specific setup is needed:
  the UPS describes its own data, and this component reads that description.
- A cable from the UPS's USB port to the ESP32's **USB host / OTG port**. On an ESP32-S3 DevKitC
  that is the port labelled `USB`, not `UART`. You'll usually need a USB-A female to USB-C adapter.

> **Power:** the ESP32 has to power the UPS's USB connection. On the ESP32-S3-DevKitC, power the
> board through its `UART` port or the 5V pin, and close the `USB-OTG` solder jumper (or use a
> powered hub) so 5V reaches the `USB` port. Check your board's documentation.

## Quick start

```yaml
esphome:
  name: ups-nut

esp32:
  board: esp32-s3-devkitc-1

# Everything UPS-related: reading the UPS, the NUT server, and all entities
packages:
  ups: github://jamesgoodhouse/esphome-nut/packages/ups.yaml@main

logger:

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password

ota:
  - platform: esphome

nut_server:
  users:
    - username: upsmon
      password: !secret nut_password
      role: primary
```

The package sets everything up. Anything you add under `ups_hid:` or `nut_server:` is merged
with it (see [Customising the package](#customising-the-package)).

Flash it, plug in the UPS, and check the logs:

```
[I][ups_hid]: USB device 051D:0002 connected (American Power Conversion Back-UPS ES 600M1)
[I][ups_hid]: Recognised UPS: 14 values and status flags from 9 reports
[I][ups_hid]: UPS ready: American Power Conversion Back-UPS ES 600M1, status: OL CHRG
[I][nut_server]: Listening on port 3493
```

Then from any computer with NUT installed: `upsc ups@<esp32-ip>`.

More examples: [minimal](examples/minimal.yaml), [Synology](examples/synology.yaml),
[every option](examples/full.yaml).

## Connecting clients

Clients connect to **`ups@<esp32-ip>`** (port 3493). Give the ESP32 a fixed IP (`manual_ip:` or a
DHCP reservation in your router), because most clients store the address.

### Synology DSM

Synology's UPS client always uses the UPS name `ups` and the login `monuser` / `secret`, and
neither can be changed. Turn on:

```yaml
nut_server:
  synology_compatible: true
```

Then on the NAS: **Control Panel → Hardware & Power → UPS**. Enable UPS support, set the type to
**Synology UPS server** and enter the ESP32's IP.

`monuser` / `secret` is a well-known login, so it is added as a **secondary** user: it can read
status but can't force other machines to shut down.

### Linux / macOS upsmon

In `upsmon.conf`:

```
MONITOR ups@192.168.1.50 1 upsmon <password> primary
```

The username, password and `primary` / `secondary` must match a user in `nut_server: users:`.

### Home Assistant

Add the **Network UPS Tools (NUT)** integration with the ESP32's IP and port 3493. You can also
use the ESPHome entities below instead, or both.

## Works without Home Assistant

ESPHome normally reboots a device that hasn't heard from Home Assistant for 15 minutes. For a UPS
monitor that's the wrong behaviour: if Home Assistant runs on your NAS and the NAS shuts down
during an outage, the ESP32 would reboot every 15 minutes in the middle of it, and NUT clients
treat a lost UPS on battery as an emergency.

So when `nut_server` is used, that reboot is **turned off automatically**. The boot log says so:

```
[I][nut_server]: Home Assistant is optional: this device will not reboot when Home Assistant is unreachable, ...
```

To get ESPHome's normal behaviour back, set `reboot_if_home_assistant_unreachable: true`. If you
leave out `api:` entirely, there is nothing to turn off.

## Configuration

### `ups_hid`

| Option | Default | What it does |
|---|---|---|
| `update_interval` | `15s` | How often to read the UPS. Status changes the UPS pushes (power lost, battery low) arrive immediately. |

### `nut_server`

| Option | Default | What it does |
|---|---|---|
| `ups_name` | `ups` | Clients connect to `<ups_name>@<esp32-ip>`. |
| `ups_description` | UPS make and model | Shown to clients. |
| `port` | `3493` | The standard NUT port. |
| `max_connections` | `4` | How many clients can connect at once (up to 8). |
| `require_login` | `false` | `false`: anyone on your network can read UPS status (stock NUT behaviour). `true`: only listed users. |
| `users` | none | Logins for clients. Each has `username`, `password` and `role`. |
| `users: role` | `secondary` | `primary`: the one machine allowed to tell everyone else to shut down. `secondary`: monitors the UPS and shuts itself down. |
| `synology_compatible` | `false` | Adds Synology's built-in login (`monuser` / `secret`) as a secondary user. Requires `ups_name: ups`. |
| `reboot_if_home_assistant_unreachable` | `false` | See [Works without Home Assistant](#works-without-home-assistant). |
| `ups_hid_id` | automatic | Which UPS to serve; only needed if you have more than one. |

There are no default users. With no users, read-only tools like `upsc` and Home Assistant still
work, but `upsmon` needs a login.

### Customising the package

```yaml
substitutions:
  ups_prefix: Office UPS        # entity names become "Office UPS Battery charge", ...

nut_server:
  synology_compatible: true     # any ups_hid / nut_server option is merged in

sensor:
  - id: !extend ups_sensors     # also ups_binary_sensors, ups_text_sensors
    wall_frequency: !remove     # drop an entity you don't want

# nut_server: !remove           # sensors only, no NUT server
```

Every entity has an id (`ups_battery_charge`, `ups_on_battery`, `ups_status`, ...) for use in
automations.

### ESPHome entities without the package

If you'd rather not use the package, add `external_components`, `ups_hid:` and `nut_server:`
yourself (see [examples/full.yaml](examples/full.yaml)) and list the entities you want:

```yaml
sensor:
  - platform: ups_hid
    battery_charge: { name: Battery charge }                        # %
    battery_runtime_remaining: { name: Battery runtime remaining }  # seconds
    battery_voltage: { name: Battery voltage }
    wall_voltage: { name: Wall voltage }        # into the UPS
    output_voltage: { name: Output voltage }    # out to your equipment
    wall_frequency: { name: Wall frequency }
    load_percent: { name: Load }                # % of the UPS's capacity in use

binary_sensor:
  - platform: ups_hid
    ups_connected: { name: UPS connected }
    on_battery: { name: On battery }
    charging: { name: Charging }
    low_battery: { name: Low battery }
    battery_needs_replacement: { name: Battery needs replacement }
    overloaded: { name: Overloaded }

text_sensor:
  - platform: ups_hid
    status: { name: Status }            # "On battery, discharging"
    status_code: { name: Status code }  # "OB DISCHRG"
    manufacturer: { name: Manufacturer }
    model: { name: Model }
    serial_number: { name: Serial number }
```

A UPS only reports what it supports, so some values may stay empty.

## How it behaves when things go wrong

| Situation | What you'll see |
|---|---|
| Board without USB host (classic ESP32) | ESPHome refuses the config and explains why. |
| Nothing plugged in | Warning "No UPS connected". NUT clients get `ERR DRIVER-NOT-CONNECTED`. |
| Plugged-in device isn't a UPS | Error in the log naming the device and why it was rejected. |
| UPS stops answering | After about 4× `update_interval`, NUT clients get `ERR DATA-STALE`. |
| UPS unplugged | `UPS connected` turns off and NUT clients get `ERR DRIVER-NOT-CONNECTED`. |

### Forced shutdown (FSD)

When the `primary` client decides it's time to shut everything down, it sends FSD. Every client
then sees `FSD` in the status and shuts down. The flag clears once the UPS has been back on wall
power for 5 minutes, or when the ESP32 restarts.

## Not supported

- Changing UPS settings or sending commands (beeper off, self-test, ...). The server is read-only.
- UPSes that only have a serial (RS-232) port.
- TLS (`STARTTLS`).

## If your UPS isn't recognised

1. Set `logger: level: VERBOSE` and plug in the UPS.
2. Copy the `Report descriptor dump` lines from the log.
3. Open an issue with the dump, the UPS model, and what the UPS's display or vendor software
   shows for battery charge and voltage.

If a UPS reports a value wrongly (for example a battery voltage 1.5× too high), the fix goes in
[quirks.cpp](components/ups_hid/quirks.cpp).

## Development

The descriptor parser, the HID → NUT mapping and the NUT protocol are plain C++ with host tests:

```sh
make -C tests
```

Real descriptor dumps saved as `tests/fixtures/<name>.hex` (whitespace-separated hex bytes) are
picked up by the tests automatically.

To try local changes on a device while using the package, add a local source. It takes precedence
over the GitHub copy the package pulls in:

```yaml
external_components:
  - source:
      type: local
      path: /path/to/esphome-nut/components
```
