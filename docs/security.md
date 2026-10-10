# Deployment security boundaries

## Development board

The firmware verifies server identity and certificate dates, uses a CA-issued device
identity, and requires a per-device proof of possession for new BLE enrollment.
The maintenance tool creates `spiffs_root/provisioning.pop` with mode 0600 when
explicitly requested. Transfer its value to the ESP BLE Provisioning app through
a trusted local channel; do not put it in Git, serial logs, or shared screenshots.
Existing Wi-Fi provisioning is preserved when firmware is updated.

This does not protect a private key from physical flash extraction. SPIFFS stores
the key in plaintext, and this development build has neither Secure Boot nor
Flash Encryption enabled.

## OTA trust and recovery

Firmware packages arrive through the existing mutually authenticated X.509
DTLS session with ThingsBoard. Anjay implements Object 5 binary push; there is
no HTTP/CoAP Package URI downloader or separate unauthenticated update listener.
ESP-IDF checks the application image before the boot partition is changed.
The application also checks the ESP32 target, available slot space, image
length and project identity. See [OTA](ota.md) for the accepted artifact.

These images have no independent firmware signature. DTLS authenticates the
configured server and protects the transfer, while the ESP image checksum/hash
detects corruption. Neither supplies an independent publisher identity when
ThingsBoard or its trusted server credentials are compromised. Restrict OTA
package upload and assignment to authorized ThingsBoard users. The server is
trusted to select the correct device configuration and application behavior.

Rollback is a recovery mechanism, not anti-rollback security. A candidate is
confirmed only after local startup and authenticated LwM2M registration within
180 seconds of application startup. Network or server outages during that
window can reject an otherwise functional candidate. Confirmation does not
prove sensor accuracy, telemetry persistence or long-term application health.
Older firmware with a different version can still be installed; no eFuse
security-version policy or independent signature enforcement is enabled.

OTA preserves the partition table, NVS Wi-Fi credentials, SPIFFS credentials and
PoP. Build-time endpoint and network settings are part of the new application;
they must still match the intended device. Bootloader, partition changes and
certificate renewal use USB. Keep a verified USB recovery bundle available.

## Before enabling hardware security

ESP-IDF's SPIFFS driver rejects encrypted partitions. Adding `encrypted` to the
current SPIFFS partition is not a working key-protection solution. Before factory
deployment, migrate device secrets to supported encrypted storage (for example,
encrypted NVS with a properly provisioned key partition, or encrypted FAT), or
use a suitable secure element and key interface. That changes storage layout and
manufacturing and is a separate migration, not a configuration toggle.

Also define the trusted firmware signing process, key custody/backups, recovery
method, debug/download-port policy, and test secure-boot/flash-encryption on a
disposable board of the same chip revision. Confirm the chosen workflow supports
certificate renewal and firmware recovery after interrupted power.

No script in this repository burns eFuses, generates production signing keys,
enables irreversible protections, or provisions external CA accounts.
Hardware-security provisioning requires a separate manufacturing and recovery
process. The USB tool supports only the unencrypted development layout; do not
use it to service a hardware-secured production device.

`python tools/device.py production-check --device-name esp32-dht11-01` reports
the unmet requirements and exits unsuccessfully for this development layout,
even if hardware-security config flags are enabled. It never changes the board.

## Renewal and revocation

Replace a device certificate before expiry using the USB workflow in
[maintenance.md](maintenance.md). Keep the CA's recovery policy intact; do not
copy the CA private key onto the ESP32 or development machine. Revoke the old
device identity only after verifying the replacement, according to the selected
CA/ThingsBoard policy. A successful TLS handshake alone does not prove that
ThingsBoard enforces CA revocation; test that server policy separately.

SNTP is not authenticated. Use a controlled internal NTP source when the network
threat model requires trusted time distribution. BLE proof of possession limits
enrollment to someone possessing the per-device secret, but does not replace
physical access controls or encrypted key storage.
