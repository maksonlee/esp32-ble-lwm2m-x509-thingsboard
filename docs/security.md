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
