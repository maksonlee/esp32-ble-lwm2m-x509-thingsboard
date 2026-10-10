# ESP32 BLE provisioning and LwM2M for ThingsBoard

ESP-IDF firmware for an original ESP32 with 4 MiB flash and a DHT11 on GPIO 23.
It provisions Wi-Fi over BLE and reports temperature and humidity to ThingsBoard
using Anjay, LwM2M 1.1 and X.509 DTLS.

Features include per-device BLE proof of possession, time synchronization before
DTLS, BOOT long-press Wi-Fi reset, USB firmware and certificate maintenance, and
ThingsBoard firmware updates through LwM2M Object 5. OTA uses the existing two
application slots and ESP-IDF rollback. Device credentials are retained during
Wi-Fi reprovisioning and OTA.

**Licensing:** application code is MIT; Anjay/avs_coap use AVSystem's
Non-Commercial License. Commercial use needs a separate AVSystem license.
See [third-party notices](THIRD_PARTY_NOTICES.md) before using or distributing
the linked firmware.

## Requirements

- Original ESP32 with 4 MiB flash; DHT11 data on GPIO 23, a common ground and a
  suitable pull-up for the sensor/module. The sampling interval defaults to five
  seconds after each completed read.
- ESP-IDF **6.1** and its ESP32 toolchain, with the IDF Python environment active.
- Git, CMake 3.22+, Ninja and OpenSSL 3. Host tests also need a C compiler with
  AddressSanitizer/UndefinedBehaviorSanitizer and OpenSSL development headers.
  The documented maintenance/test workflow is verified on Linux.
- A ThingsBoard LwM2M server with an ECDSA server identity and a trusted device CA,
  a matching device certificate/key, and network access to DTLS and NTP.
- A BLE provisioning client supporting Espressif Security 1 and a per-device PoP.

ESP-IDF 6.1 support uses local compatibility patches; the upstream ESP-IDF
adapter is archived. See [compatibility](docs/compatibility.md) for exact source
revisions, patch rationale and the qualified configuration.

Native X.509 device auto-creation was verified with **ThingsBoard CE 4.4.0 plus
[PR #16179](https://github.com/thingsboard/thingsboard/pull/16179)**, applied to
both the core and LwM2M transport. The PR was still open at validation time;
these results do not establish support in an unpatched 4.4.0 installation.
ThingsBoard 4.3.1.5 was tested with a pre-created device instead. See
[server setup](docs/thingsboard.md) for both modes.

## Configure and build

Run from the repository root in an activated ESP-IDF 6.1 environment.

```sh
python tools/prepare_anjay.py
idf.py set-target esp32
idf.py menuconfig
idf.py build
```

Under **Application Configuration**, set:

| Setting | Value |
| --- | --- |
| LwM2M server URI | Your secure endpoint, for example coaps://thingsboard.example.com:5686 |
| LwM2M endpoint | The exact device certificate CN and ThingsBoard endpoint |
| DHT11 data GPIO | 23 by default |
| Telemetry interval | 5 seconds by default |
| Time server | A reachable NTP server |

The example hostname is deliberately non-operational. Replace it before flashing.
Settings are saved in ignored `sdkconfig`. Dependency downloads, managed
components, build output and credentials are also excluded from Git.
Application builds do not read or embed device credentials.

OTA requires `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`,
`CONFIG_ANJAY_WITH_MODULE_FW_UPDATE=y` and `CONFIG_ANJAY_WITH_DOWNLOADER=n`.
Check existing saved configurations as well as new builds; `sdkconfig.defaults`
does not override saved settings. The initial USB installation must include
the rebuilt bootloader. See [OTA setup](docs/ota.md) before preparing an update.

## Configure ThingsBoard and flash over USB

Follow [ThingsBoard setup](docs/thingsboard.md) to configure X.509 CA/CN
authentication, the secure listener, and
[temperature/humidity mappings](thingsboard/telemetry-mapping.json).

Prepare a private directory containing these files:

| File | Content |
| --- | --- |
| device.crt | PEM device certificate, intermediate(s), then issuing root |
| device.key | Matching PEM private key, mode 0600 |
| root_ca.crt | PEM trust anchors for the server; these may differ from the device CA |
| provisioning.pop | Per-device URL-safe enrollment secret, mode 0600, no trailing newline |

Use directory mode 0700. The device leaf must explicitly allow TLS client
authentication. The USB tool validates the chain, CN, key, permissions, expiry,
firmware endpoint and target MAC.

```sh
python tools/device.py check --device-name esp32-dht11-01 \
  --certs-dir /private/device-bundle
python tools/device.py flash --device-name esp32-dht11-01 \
  --certs-dir /private/device-bundle --port /dev/ttyUSB0 \
  --expected-mac YOUR_BOARD_MAC
```

Replace the example device name with the configured endpoint and
`YOUR_BOARD_MAC` with the intended board's MAC. Add `--generate-pop` only for a
new bundle that has no PoP; retain the existing secret when updating a device.
Flashing generates the protected SPIFFS image and preserves Wi-Fi NVS.

For first enrollment, connect to the advertised `PROV_xxxxxx` BLE device and
supply its PoP through a trusted local channel. The secret is not printed.
After normal startup, release BOOT and then hold it for five seconds to clear
Wi-Fi settings and re-enter provisioning. Device credentials and PoP are retained.
See [maintenance](docs/maintenance.md) for certificate-only updates and recovery,
and [security boundaries](docs/security.md) for plaintext SPIFFS storage limits.

## Firmware updates over LwM2M

Follow [OTA setup and recovery](docs/ota.md) to configure ThingsBoard's native
Object 5 binary push and upload the raw `build/wifi_prov_mgr.bin` application.
Use firmware versions `v01`, `v02`, `v03`, and so on (`v02` is the current default).
Commit and tag each release before building its OTA package; `v01` is the baseline.
Build each package with a distinct version and the intended device's endpoint
and configuration. Each application slot is `0x1d0000` bytes; OTA does not update
the bootloader, partition table or device credentials.

A candidate must complete local startup and register with ThingsBoard over
X.509 DTLS within 180 seconds of application startup. Otherwise it rolls back
to the previous application. This version uses DTLS and ESP-IDF image integrity
checks; independent firmware signing and Secure Boot remain outside its scope.

## Verification

```sh
python -m unittest discover -s tests -v
```

Run after the application build. [Test instructions](tests/README.md) also cover
the isolated ESP32 QEMU DTLS suite, which needs no board or server credentials.
Tests use synthetic identities and a loopback peer; they do not access a live
ThingsBoard deployment.

Physical validation covered X.509 DTLS, registration, five-second sensor delivery
including equal readings, registration Update, and restart recovery. Automatic
creation and same-device reconnection passed with Linux Anjay and ESP32 clients
on ThingsBoard 4.4.0 with PR #16179. BOOT-triggered BLE reprovisioning passed with
a host BLE client, including rejected enrollment credentials, recovery and
retention of device credentials and PoP.
Mobile provisioning apps and extended failure/recovery remain unverified.
OTA transfer, power interruption and rollback on physical hardware with
ThingsBoard require separate validation; earlier sensor/provisioning checks
do not qualify OTA.
See [test coverage and limitations](tests/README.md) for details.

## Repository layout

```text
main/                     Application (one ESP-IDF component)
  app_main.c              Startup and task initialization
  lwm2m/                  Anjay client, sensor objects and firmware-update object
  network/                BLE Wi-Fi provisioning and time synchronization
  sensors/                DHT11 driver and frame decoder
  storage/                SPIFFS access and certificate loading
  maintenance/            BOOT button, OTA flash writes and boot confirmation
components/anjay/         SDK integration and compatibility patches
tools/                    Dependency preparation and USB maintenance
tests/                    Host checks and ESP32 DTLS regression tests
thingsboard/              Sensor mapping configuration
docs/                     Compatibility, server setup, OTA, maintenance and security
licenses/                 Retained upstream license texts
```

`main/CMakeLists.txt`, `main/Kconfig.projbuild` and `main/idf_component.yml`
keep application build settings together. Each module's source and header live
in the same functional directory.

The application derives from the [MQTT project](https://github.com/maksonlee/esp32-ble-mqtt-x509-thingsboard)
at commit `0458252`. See [source attribution](THIRD_PARTY_NOTICES.md#application-source)
for reused modules and license details.
