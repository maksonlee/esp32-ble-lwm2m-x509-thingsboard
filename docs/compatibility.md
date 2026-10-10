# ESP-IDF and LwM2M compatibility

## Tested configuration

The application targets the original ESP32 with 4 MiB flash, ESP-IDF 6.1 and
Anjay 3.15.0. Existing-device communication was tested against ThingsBoard CE
4.3.1.5 and 4.4.0 with
[PR #16179](https://github.com/thingsboard/thingsboard/pull/16179). Native X.509
auto-creation was verified with a Linux Anjay client and a separate new RSA
identity on the ESP32 against the patched 4.4.0 server. Both clients passed
same-device reconnection and delivered fresh temperature/humidity telemetry.
The application uses X.509 DTLS, LwM2M 1.1 and standard Temperature/Humidity
objects. OTA adds Anjay's Firmware Update Object 5 (object version 1.0) and a
minimal Device Object 3. Neither object advertises an explicit version;
ThingsBoard's Leshan 2.0.0-M15 resolves Device Object 3 as version 1.1 and
Firmware Update Object 5 as version 1.0 for LwM2M 1.1 registration. Use the
corresponding versioned paths in [OTA mappings and RPC](ota.md). A physical
`v01` to `v02` update passed transfer, automatic installation, boot confirmation
and fresh telemetry. Power interruption and physical rollback remain unverified.

Physical regression checks also passed two BOOT-triggered BLE Wi-Fi
reprovisioning cycles with Espressif Security 1 and a host BLE client. Device
certificates, keys, trust roots and PoP were retained, and LwM2M telemetry resumed.
Mobile provisioning apps were not independently tested. See
[validation scope](../tests/README.md#hardware-validation-scope) for the checks
and remaining limits.

Anjay core is used with local compatibility changes. The upstream ESP32 example
and ESP-IDF adapter are archived; their documented ESP-IDF 5.3.1 testing does
not establish official support for 6.1. Other ESP32 variants and SDK releases
are not covered by this repository's results.

| Input | Pinned revision |
| --- | --- |
| [Anjay 3.15.0](https://github.com/AVSystem/Anjay/tree/3.15.0) | fdd70854c46f676acda179ad3a1b760eceada4db |
| [avs_commons 5.9.0](https://github.com/AVSystem/avs_commons/tree/e6c87eb58b3d605b1bd26eff816f3ad4b366b993) | e6c87eb58b3d605b1bd26eff816f3ad4b366b993 |
| [Anjay-esp-idf](https://github.com/AVSystem/Anjay-esp-idf/tree/0c910a1aa0c812d1f4d1eb75b80fbce48241aef1) | 0c910a1aa0c812d1f4d1eb75b80fbce48241aef1 |
| [ESP-IDF v6.1](https://github.com/espressif/esp-idf/tree/v6.1) | fff9895c82d744c7237be8847347bdd1b07c6643 |

`tools/prepare_anjay.py` verifies revisions, rejects unexpected dependency edits,
and applies the patches in `components/anjay/patches/` idempotently. Application
and emulator tests use the same dependency trees, component and patches.
The SDK and Anjay core protocol code are unmodified.

## Mbed TLS 4 changes

ESP-IDF 6.1 bundles Mbed TLS 4.1.0 and TF-PSA-Crypto. The avs_commons patch:

- Uses PSA RNG and avoids removed public entropy/CTR-DRBG APIs.
- Adapts private-key parsing and pair-checking calls to the new signatures.
- Compares exported public SubjectPublicKeyInfo DER for RSA keys. The SDK's
  PKCS#8 parser can leave a cached public key empty, causing the native pair
  check to reject an otherwise matching certificate/key. Export failures and
  actual mismatches still fail. No private crypto header is added for this fix.
- Guards removed key-exchange enum values and uses conservative DTLS record
  expansion for payload limits.

The adapter configuration selects PSA RNG and disables unused PSK support.
The component selects GNU C99 because GCC 15 otherwise defaults to C23. It
retains the reference adapter's alignment-assertion workaround only inside
Anjay; application/test assertions remain enabled. HTTP and download support
are disabled, so the legacy HTTP Digest MD5 stream library is excluded.
Anjay's firmware-update module is enabled for CoAP block-wise push through
Object 5; URI pull remains disabled. The adapter patch removes its unnecessary
Kconfig dependency on the downloader so push can be enabled independently.
The application streams the package into
ESP-IDF's inactive OTA partition with `esp_ota_begin`, `esp_ota_write` and
`esp_ota_end`, then selects it only after a valid Execute request. ESP-IDF's
rollback-enabled bootloader and application confirmation use the existing
partition layout. No dependency revisions are changed for OTA.

These patches qualify the tested configuration, not every crypto feature,
session-resumption path or downstream application. See [tests](../tests/README.md)
for ECDSA/RSA mutual DTLS and negative certificate checks.

## ThingsBoard interoperability

The firmware selects LwM2M 1.1. Anjay's 1.0 registration quotes object versions
as `ver="1.1"`; the tested Leshan parser returns 4.00 for that encoding.
Native 1.1 emits the accepted unquoted form. TLV and SenML CBOR are enabled;
the independent wire test checks the version and object links explicitly.

ThingsBoard's configured server suites require an ECDSA server identity:
`TLS_ECDHE_ECDSA_WITH_AES_128_CCM_8` or
`TLS_ECDHE_ECDSA_WITH_AES_128_CBC_SHA256`. An RSA client identity works through
the pre-created CA-trust/CN path tested on 4.3.1.5 and the native auto-creation
path tested on patched 4.4.0. The latter stores structured `LWM2M_CREDENTIALS`
with X509 client security, the full CN endpoint and the pinned RSA leaf.

The native auto-creation results use PR revision
`9a8ebae83ccebdd6eda97c22f227c3fa5e746063`, applied to both the core and LwM2M
transport. The PR was still open at validation time; an unpatched 4.4.0 release
is not qualified by these results. Its profile uses X509 Certificates Chain,
Create new devices and a CA included in the transmitted device chain. No
bootstrap connection is used. See [server setup](thingsboard.md) for identity
mapping and the pre-created endpoint alternative.

MQTT authentication alone does not prove that the same server identity, trust
roots or client-credential mode will work for DTLS.

ThingsBoard's native Object 5 binary strategy and automatic Execute on the
Downloaded state were reviewed in the 4.4.0-SNAPSHOT transport source and passed
the physical `v01` to `v02` test. This validates the normal update path;
failure and rollback coverage remains limited to the automated tests described
in [test coverage](../tests/README.md). The required model versions, profile
observations, package metadata and recovery policy are described in
[OTA setup](ota.md).

Dependency licensing is described in [third-party notices](../THIRD_PARTY_NOTICES.md).
