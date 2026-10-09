# Third-party notices

The repository's [MIT license](LICENSE) does not replace dependency licenses.
Dependencies are fetched during preparation/build and are not vendored here.

| Component | License | Use in this repository |
| --- | --- | --- |
| MQTT application at commit 0458252 | MIT, as declared in its README | Reused application and maintenance code; see [application source](#application-source) |
| Anjay 3.15.0 and its bundled avs_coap | AVSystem Anjay Non-Commercial License | Downloaded LwM2M/CoAP implementation |
| avs_commons 5.9.0 | Apache-2.0 | Downloaded crypto/network backend; modified by the local patch |
| Anjay-esp-idf adapter | Apache-2.0 | Downloaded configuration; adapted component integration and configuration patch |
| ESP-IDF 6.1 | Apache-2.0 and bundled component licenses | SDK, including Mbed TLS/TF-PSA-Crypto |
| espressif/network_provisioning 1.2.4 | Apache-2.0 | Managed BLE provisioning dependency |
| espressif/cjson 1.7.19~2 | MIT | Managed provisioning dependency |

## Application source

The application derives from [maksonlee's ESP32 MQTT project](https://github.com/maksonlee/esp32-ble-mqtt-x509-thingsboard),
branch `idf-6.x`, commit `04582528d5c36f22ff9a162c8e8029a99777bbc6`.
That revision's README declares MIT licensing. Reused modules include BLE Wi-Fi
provisioning, DHT11/RMT decoding, BOOT button handling, SPIFFS and certificate
loading, time synchronization and USB maintenance. MQTT integration was replaced
with LwM2M; the partition table is unchanged.

The LwM2M sensor objects use Anjay's public data model API and the OMA
[Temperature](https://raw.githubusercontent.com/OpenMobileAlliance/lwm2m-registry/prod/3303.xml)
and [Humidity](https://raw.githubusercontent.com/OpenMobileAlliance/lwm2m-registry/prod/3304.xml)
definitions.

## AVSystem licensing

The pinned Anjay/avs_coap license permits the non-commercial uses it defines.
Commercial use requires a separate AVSystem license. Publishing this project's
source does not grant unrestricted commercial rights to its dependencies or
the linked firmware. Read the [exact pinned license](licenses/AVSystem-Anjay.txt)
and [upstream terms](https://github.com/AVSystem/Anjay/blob/3.15.0/LICENSE).

This product includes software developed by AVSystem Sp. z o.o.

Retained upstream attribution:

- Anjay-esp-idf: Copyright 2023-2025 AVSystem.
- AVSystem Commons Library: Copyright 2017-2020 AVSystem. Individual modified
  files retain their additional copyright notices, including 2026 notices.
- AVSystem CoAP Library: Copyright 2017-2026 AVSystem.
- This product includes software developed at AVSystem (www.avsystem.com).

The local integration changes select GNU C99, the SDK crypto dependencies,
PSA RNG and the supported Mbed TLS 4 APIs. The patches also handle RSA PKCS#8
public-key comparison, removed enum values and conservative DTLS record sizing.
Their scope and pinned source revisions are recorded in
[compatibility](docs/compatibility.md). The shared component CMake file retains
its upstream Apache notice. The [Apache-2.0 license](licenses/Apache-2.0.txt)
applies to these adapted portions and patches.

The adapter's cellular example and FreeRTOS Cellular Interface are not used or
redistributed. Complete dependency notices remain in the fetched source trees.
For binary distribution, include the applicable dependency licenses/notices
and satisfy their terms; the application MIT license alone is insufficient.
