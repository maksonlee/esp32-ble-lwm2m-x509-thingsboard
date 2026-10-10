# Tests

Run from an activated ESP-IDF 6.1 environment. The host checks require Linux,
Python, Git, CMake, Ninja, OpenSSL 3 development headers and a C compiler with
ASan/UBSan.
No test command below flashes a board or contacts a live ThingsBoard device.

Native compiler subprocesses select the host compiler and prioritize its binary
directory for binutils. This prevents ESP-IDF's unprefixed ULP linker from being
used to link host executables when the SDK environment is active.

## Application and host checks

From the repository root:

```sh
python tools/prepare_anjay.py
idf.py build
python -m unittest discover -s tests -v
```

The build contracts inspect the root application's generated `build/` files.
Other checks compile the actual C implementation against failure-injection
substitutes and test USB safeguards with mocks. Disposable PKI fixtures are
generated in temporary directories and removed when each test completes.

The application suite covers:

| Area | Tests | Coverage |
| --- | --- | --- |
| Build configuration | 5 | SDK/target, BLE/DTLS, certificate dates, partition layout, image size and sensor settings |
| Native C modules | 3 | File-read failures, DHT11 frame decoding and BOOT hold/release behavior with ASan/UBSan |
| Certificates | 9 | Chain order, key/CN matching, validity, permissions, PEM content, symlinks and independent server trust |
| USB maintenance | 5 | Target identity, partition compatibility, certificate-only writes, storage requirements and endpoint matching |
| LwM2M | 2 | Startup failures, registration gating, disconnect races, sampling intervals and CoAP/TLV observations |
| OTA platform | 6 | Split image headers, target/version checks, size/integrity, Flash failures, NVS ordering, restart recovery and confirmation deadlines with ASan/UBSan |
| OTA wire | 7 | Real Anjay Object 3/5, CoAP Block1 push/retransmission/interruption, cancellation, error mapping, Execute and persistent-result handoff |

Focused checks can be run with, for example:

```sh
python -m unittest discover -s tests -p test_lwm2m.py -v
```

The LwM2M test builds its OpenSSL-backed Anjay directly from the root pinned
sources. Its NoSec UDP peer is restricted to 127.0.0.1. It checks registration
version/object links and actual TLV Observe notifications, equal successive
readings, failed-read suppression and recovery. Firmware requires X.509 DTLS.

## OTA checks

Run the focused checks before the full application suite:

```sh
python -m unittest discover -s tests -p test_ota.py -v
python -m unittest discover -s tests -p test_ota_wire.py -v
```

`tests/ota/` compiles the actual `main/maintenance/firmware_update.c` against
isolated ESP-IDF substitutes. It verifies arbitrary header chunk boundaries,
rejection before Flash writes, capacity and exact image length, SDK failure
cleanup, cancellation, disconnected transfers, retained downloaded packages,
and retry after a failed Execute. Simulated cold boots retain only fake NVS:
tests check journal commit before boot selection, rollback reporting, and
mark-valid before success persistence. The independent 180-second boot deadline
is tested without a network; local startup and registration must permit
confirmation before it expires. These tests exercise application decisions;
the substitutes do not validate a real ESP32 binary or physically erase Flash.

The OTA wire test links the actual LwM2M bridge with pinned Anjay and a fake
platform. Its independent loopback peer checks Object 3 version/binding and
Object 5 state/results, push-only delivery, rejected URI pull, exact transferred
bytes, duplicate Block1 packets, cancellation and invalid-image/space/memory
failures. It checks that Anjay's cleanup callbacks preserve an update result
reported after reboot. The transport is NoSec on 127.0.0.1 only; application
firmware continues to require X.509 DTLS.

Application build contracts require the rollback-enabled bootloader and
push-only firmware-update module and verify both unchanged slot sizes. The
ESP32 DTLS emulator suite below tests crypto separately; it does not qualify
OTA Flash writes, boot selection or rollback.

On 2026-10-10, a separate physical/server check passed the normal `v01` to `v02`
update on the original ESP32 with 4 MiB flash and ESP-IDF 6.1, using ThingsBoard's
4.4.0-SNAPSHOT LwM2M transport. It began with a full `v01` USB installation,
including the rollback-enabled bootloader, and fresh X.509 DTLS registration
and sensor readings. ThingsBoard had the matching Device 1.1 and Firmware
Update 1.0 models and the [combined OTA observations](../thingsboard/ota-telemetry-mapping.json).

Assigning the `v02` package to that device transferred the application and
automatically executed installation. Serial output confirmed `v02` running in
`ota_1` and firmware confirmation after server registration. Fresh LwM2M Reads
reported running version `v02`, Firmware Update State `0` and Update Result `1`.
Two fresh temperature/humidity readings followed reconnection. These results
qualify the normal physical update path; they are separate from the host tests
and do not establish physical rollback behavior.

Still validate malformed-image rejection, transfer interruption, power loss
before/after boot selection, candidate startup failure, the 180-second
network/NTP/registration deadline, rollback reporting and a later successful
retry on hardware. Compare retained Wi-Fi configuration, certificate bundle
and PoP through the authorized maintenance workflow. Earlier hardware results
below cover telemetry/provisioning and do not extend this OTA failure coverage.

## ESP32 DTLS regression suite

This is a separate ESP-IDF test project under `tests/esp32_dtls/`, sharing
`components/anjay/`, its patches, the root `.deps/` and the application partition
table. It does not include BLE/Wi-Fi application provisioning or physical sensors.

Prepare root dependencies as above, then:

```sh
cd tests/esp32_dtls
idf.py build
python run_qemu.py --qemu /path/to/qemu-system-xtensa
```

Install the Espressif QEMU version supported by the SDK, along with its host
shared-library dependencies. The tested QEMU version is Espressif
`esp-develop-9.2.2-20260417`, listed in ESP-IDF 6.1's `tools/tools.json`.
The [official Linux AMD64 archive](https://github.com/espressif/qemu/releases/download/esp-develop-9.2.2-20260417/qemu-xtensa-softmmu-esp_develop_9.2.2_20260417-x86_64-linux-gnu.tar.xz)
has SHA-256 `0eecb2a34a5586c0e59110f77b9343b7b336e82fdb0e1a30e1dc1bab8a547e35`.

The runner merges only generated test images, disables host networking with
`-nic none`, imposes a timeout and requires `PROBE: ALL PASS`. It terminates
the emulator and never opens a serial port.

The suite checks:

- Anjay create/delete and Security/Server object installation.
- Mutual X.509 DTLS with ECDSA identities using CCM-8 and CBC-SHA256 suites.
- Echoed datagrams at the backend's reported payload limit.
- Rejection of an untrusted server root, wrong server hostname, expired server
  certificate and untrusted device certificate, with the expected verify flags.
- PKCS#8 RSA 2048 client authentication with both ECDHE-ECDSA server suites.
- Rejection of a different RSA private key for the same client certificate.

The fixed emulator clock, runtime fixture issuance and local server belong only
to tests. RSA/ECDSA fixture keys exist in emulator RAM and are wiped; no actual
device credentials are read or saved. The transport test disables LwM2M 1.1
because it tests DTLS directly; the application and host wire test use 1.1.

These checks do not qualify every SDK feature, TLS resumption, hardware-secured
storage or prolonged field operation.

## Server automatic-provisioning validation

Separate manual integration checks used a native Linux Anjay 3.15.0 client
against ThingsBoard CE 4.4.0 with
[PR #16179](https://github.com/thingsboard/thingsboard/pull/16179), revision
`9a8ebae83ccebdd6eda97c22f227c3fa5e746063`, applied to the core and LwM2M
transport. These checks are not part of the automated test commands above.

The profile used X509 Certificates Chain with Create new devices enabled. A new
RSA identity transmitted the profile's issuing CA in its certificate chain and
used the full leaf CN as its LwM2M endpoint. The checks confirmed:

- Automatic creation without a prior device record or an administration API call
  to create it.
- Stored `LWM2M_CREDENTIALS` with X509 client security, the matching endpoint
  and pinned leaf certificate.
- LwM2M registration and fresh temperature/humidity telemetry on the first
  connection and after reconnection.
- The same device ID and unchanged credentials after reconnection.

No bootstrap connection was used. This result qualifies the patched server and
software-client boundary; it does not establish native auto-creation on an
unpatched 4.4.0 release. Physical ESP32 coverage is described below.

## Hardware validation scope

Hardware checks used an original ESP32-D0WDQ6-V3 with 4 MiB flash and a DHT11 on
GPIO 23, running ESP-IDF 6.1, Mbed TLS 4.1.0 and Anjay 3.15.0 against ThingsBoard
CE 4.3.1.5. The application build, all 24 host checks and the QEMU suite passed
with this configuration. Other boards and SDK/client versions are unverified.

Serial output and ThingsBoard telemetry confirmed:

- Saved Wi-Fi recovery, SNTP synchronization and LwM2M 1.1 registration over
  X.509 DTLS with an RSA device identity and an ECDSA server identity.
- Temperature and humidity delivery at approximately five-second intervals,
  including repeated equal readings with distinct timestamps.
- Continued sampling during registration Update and recovery after restart.
- USB firmware replacement with Wi-Fi credentials, device identity and PoP retained.

Separate server checks rejected an untrusted certificate with a matching CN and
a trusted certificate with an unknown endpoint on 4.3.1.5. That result describes
the pre-created endpoint mode, rather than the patched auto-creation path above.
See [server setup](../docs/thingsboard.md) for both authentication modes.

On ThingsBoard 4.4.0 with PR #16179, the ESP32 passed saved Wi-Fi recovery, SNTP,
X.509 DTLS registration and fresh sensor delivery with an existing identity.
A new RSA 2048 identity with an ordered leaf/issuing-CA/root chain also passed:

- First-connection creation in the configured profile with `LWM2M_CREDENTIALS`,
  X509 security, the full CN endpoint and the matching pinned leaf certificate.
- Reconnection with the same device ID and unchanged credentials.
- Real temperature/humidity delivery at approximately five-second intervals on
  both connections, including repeated equal values.

These 4.4.0 checks covered physical/server integration; the host and QEMU suites
were not rerun as part of them.

Two BOOT-triggered BLE Wi-Fi reprovisioning cycles passed with Espressif
Security 1 and a host BLE client:

- Short BOOT presses were ignored; a long hold restarted BLE provisioning.
- An incorrect PoP was rejected; the correct PoP established the secure session.
- Incorrect Wi-Fi credentials produced the firmware's credential-failure status.
  Corrected credentials connected successfully within the same secure session.
- Provisioning stopped, Bluetooth controller memory was released, SNTP completed
  and LwM2M registration resumed.
- Reprovisioning worked again after Bluetooth memory release, using the same PoP.
- Fresh telemetry resumed after each cycle with the same ThingsBoard device ID
  and unchanged device credentials. SPIFFS comparison confirmed retention of
  the certificate, key, server trust roots and PoP.

Mobile provisioning apps were not independently tested.

The following flows still need hardware validation on the LwM2M application:

- Sensor unplug/replug, prolonged Wi-Fi/server outages, DHCP renewal and cold boot without NTP.
- Long-duration memory use and interrupted certificate replacement.

Host tests cover individual failure paths, not these complete hardware flows.
See [security boundaries](../docs/security.md) for storage and deployment limits.
