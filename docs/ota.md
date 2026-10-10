# Firmware updates with ThingsBoard

OTA uses ThingsBoard's native LwM2M Firmware Update Object 5 and Anjay's binary
push handler. Packages travel through the existing X.509 DTLS session as
block-wise CoAP writes to `/5/0/0`. ESP-IDF writes the inactive application slot
and provides boot selection, image validation and rollback through its
[native OTA API](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32/api-reference/system/ota.html).

Transfers share the application's LwM2M worker with sensor access. Sensor
sampling can pause while Anjay receives the package and Flash is written;
normal sampling resumes afterward, without publishing cached readings.

The feature targets the original ESP32, 4 MiB flash and ESP-IDF 6.1. Physical
OTA transfer, power interruption and rollback against ThingsBoard have not yet
been validated. See [test coverage](../tests/README.md) for automated coverage
and the separate, earlier telemetry/provisioning hardware results.

## Install the OTA baseline over USB

1. Build with `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`,
   `CONFIG_ANJAY_WITH_MODULE_FW_UPDATE=y` and `CONFIG_ANJAY_WITH_DOWNLOADER=n`.
   Check saved `sdkconfig` files: defaults do not override saved settings.
   Keep the existing partition table and device configuration.
2. Use the full [USB maintenance workflow](maintenance.md#usb-firmware-updates-and-certificate-renewal)
   to install the application and rebuilt bootloader on the intended board.
   Retain that device's certificate bundle and PoP. Installing only the new
   application over an older bootloader does not enable rollback.
3. Verify Wi-Fi recovery, time synchronization, X.509 DTLS registration and
   fresh sensor telemetry before assigning any OTA package. Keep the validated
   baseline build and private recovery bundle available for USB recovery.

The unchanged partition table provides two `0x1d0000`-byte application slots
(1,900,544 bytes each) and the existing OTA metadata partition. NVS and SPIFFS
keep their original locations. Remote updates replace only the inactive
application; bootloader, partition table, certificates and PoP use USB.

## Build a package for the intended device

Use the same ESP32 target, partition layout and application configuration as
the installed baseline. In particular, preserve the intended
`CONFIG_LWM2M_ENDPOINT`, server URI, sensor pin and time-server settings. The
endpoint is compiled into the image and must match that device's certificate
CN; preserving SPIFFS alone does not make one binary suitable for every device.
Avoid assigning one endpoint-specific image to a shared device profile.

Name firmware versions `v01`, `v02`, `v03`, and so on. The OTA baseline is
`v01`; the current release defaults to `v02`.

Prepare each release in this order:

1. Set the `PROJECT_VER` default in `CMakeLists.txt` to the intended version,
   finish the source/documentation changes and run the applicable checks.
2. Commit the reviewed release files, then create an annotated Git tag with
   that version, for example `git tag -a v02 -m "Firmware v02"`. Create each
   release tag once and keep it on its original commit.
3. Build the final package from that tagged commit with a clean tracked
   worktree. From an activated ESP-IDF 6.1 environment, verify the commit/tag
   and explicitly select the matching firmware version:

```sh
git status --short
git rev-parse HEAD
git describe --exact-match --tags HEAD
idf.py -DPROJECT_VER=v02 build
```

Run the [release checks](../tests/README.md) on this final build before upload.
Keep the full commit ID, tag, build configuration and binary SHA-256 with the
private release artifacts. Any further source change requires a new commit
and version; an already published tag or package must not be repurposed.

Use the raw `build/wifi_prov_mgr.bin` file. Its embedded project name is
`wifi_prov_mgr`, and its embedded version is the chosen `PROJECT_VER` value.
Check the build output and package details before upload; keep the version
within ESP-IDF's 31-character application-version limit. Reusing the running
version is rejected even if the binary differs.
For the following update, set the source default to `v03`, commit/tag it, then
build with `-DPROJECT_VER=v03`. Always pass the intended
version explicitly when preparing a release; CMake remembers a previously
supplied `PROJECT_VER` in the build directory.

The receiver checks target compatibility, project name, slot capacity, exact
image length and ESP-IDF image integrity. Do not upload a merged flash image,
bootloader, partition table, SPIFFS image, archive, independently signed image
or file with additional padding. No firmware-signature verification or Secure
Boot is enabled; see [security boundaries](security.md#ota-trust-and-recovery).

## Configure the LwM2M profile

Set `profileData.transportConfiguration.observeAttr` to
[ota-telemetry-mapping.json](../thingsboard/ota-telemetry-mapping.json), preserving
the other profile settings. This combined mapping keeps the existing
temperature/humidity telemetry and whole-instance sensor observations, then
adds `/3_1.0/0/3` and the complete `/5_1.0/0` instance. It uses **Single**
observation and keeps `initAttrTelAsObsStrategy` false. Object 3 and Object 5
are version 1.0 even though the LwM2M protocol and sensor objects use version 1.1.

| Resource | Meaning |
| --- | --- |
| `/3/0/3` | Running application's embedded version |
| `/5/0/0` | Write-only binary package |
| `/5/0/2` | Execute installation of the validated package |
| `/5/0/3` | State: Idle 0, Downloading 1, Downloaded 2, Updating 3 |
| `/5/0/5` | Update Result, including Success 1 or failure |
| `/5/0/6` and `/5/0/7` | Downloaded package's embedded project name and version |
| `/5/0/9` | Delivery method: push only, value 1 |

Package name/version resources are absent until a package has been validated.
Observing the whole Object 5 instance allows them to appear when available;
an early direct Read of either can return Not Found. The minimal Device Object
also provides Reboot (`/3/0/4`), Error Code (`/3/0/11`) and UDP binding
(`/3/0/16`). Reboot is deferred until its Execute response can be sent.

Under **Device profile → Transport configuration → Other settings**:

- Select **Push firmware update as binary file using Object 5 and Resource 0
  (Package)**: `clientLwM2mSettings.fwUpdateStrategy = 1`.
- Disable **Use Object 19 for OTA file metadata**:
  `clientLwM2mSettings.useObject19ForOtaInfo = false`.
- Keep the existing X.509 security and endpoint configuration.

Object 19 metadata, Object 9 software updates and Package URI pull are not
implemented. Upload a binary package in ThingsBoard; do not assign a package
that uses an external URL.

Only temperature and humidity belong in the sensor telemetry list. Firmware
observations drive ThingsBoard's OTA state machine without adding their raw
resources to that list. ThingsBoard's native OTA service independently emits
`fw_state` and `lwm2m_log` operational telemetry; those native status records
are expected during OTA.

## Upload and assign in ThingsBoard

In ThingsBoard's OTA package manager, create a **Firmware** package for the
intended device profile and upload the raw application binary. Use:

| Package field | Value for the build above |
| --- | --- |
| Title | `wifi_prov_mgr` |
| Version | `v02` |
| Version tag | `v02` |
| File | `build/wifi_prov_mgr.bin` |

Title and Version match the embedded application descriptor. Set Version tag
to the exact running-version value that the new image will report at
`/3/0/3`; ThingsBoard uses it to recognize an already-installed package.
This ThingsBoard field and the Git release tag both use `v02`: the Git tag
identifies the source commit, while ThingsBoard compares the running version.
Inspect that resource after the update instead of inferring success from the
package assignment alone.

The startup log also reports the embedded project name, running version and
application partition, for example:

```text
app_main: System init: wifi_prov_mgr v02, running partition ota_1
```

The partition can be `ota_0` or `ota_1`, depending on the previous running slot.
Use this serial diagnostic alongside the reported version and update result.

Assign the package to the specific device during its maintenance window.
Assignment can start transfer and installation immediately. The reviewed
ThingsBoard 4.4.0-SNAPSHOT transport automatically sends Execute `/5/0/2` when
State becomes Downloaded. Its OTA behavior was reviewed in source, separately
from the physical/server validation described in the other guides.

The official [LwM2M OTA guide](https://thingsboard.io/docs/reference/lwm2m-api/ota-updates/)
also describes manual Execute. If the installed server leaves the device in
Downloaded state and does not automatically execute, the supported RPC is:

```json
{"method": "Execute", "params": {"id": "/5/0/2"}}
```

Do not send repeated Execute requests while the device is Updating or
rebooting. A downloaded package does not change the selected boot partition
until Execute. Observe the result after reconnection and verify the running
version and fresh temperature/humidity telemetry.

## Confirmation and rollback

Before switching slots, the application records the target partition and image
identity in a small NVS update journal. The new candidate starts an independent
180-second confirmation deadline early in application startup, before network
initialization. It must complete local startup and establish a fresh LwM2M
registration with ThingsBoard over X.509 DTLS before being marked valid.
Local startup requires the event loop, network/client workers and BOOT
maintenance initialization; successful registration also requires credential
loading and installation of the LwM2M objects.

The deadline includes Wi-Fi recovery, SNTP and registration. An unavailable
network, NTP source or server can cause rollback even when the image itself is
correct. Local startup checks are not a sensor-accuracy or telemetry-storage
test; a successful DHT11 measurement is not required for boot confirmation.
After confirmation, ordinary connection loss uses the existing reconnect
behavior and does not roll back a previously accepted image.

If the deadline expires, the application requests rollback and restart. A
reset or crash before confirmation is handled by the rollback-enabled
bootloader. Recovery requires the previous valid application slot. The update
journal allows the running firmware to report success after confirmation or
failure after returning to the previous application; reporting still needs a
working server connection.

| Interruption or failure | Device behavior and recovery |
| --- | --- |
| Malformed, oversized, incompatible or incomplete image | Reject the package; keep the running application's boot selection |
| Wi-Fi/client disconnect during transfer | Abort the incomplete transfer; resend the complete package |
| Disconnect after validation, before Execute | Retain the downloaded package during the same boot and report Downloaded after reconnection |
| Reboot before Execute | Discard the pending package; resend it |
| Candidate cannot confirm within 180 seconds | Roll back and report failure after reconnecting |
| Candidate crashes or resets before confirmation | Bootloader rolls back on the next boot |
| No usable previous image or USB baseline mismatch | Recover with the verified full USB installation |

An interrupted download is never resumed from a stored byte offset. Do not
erase NVS or OTA metadata to retry a failed update. Correct the package or
connectivity problem and retry through ThingsBoard; retain the current device
identity and sensor history. Hardware power-loss behavior, server retry policy
and long transfers still require validation on the intended deployment.
