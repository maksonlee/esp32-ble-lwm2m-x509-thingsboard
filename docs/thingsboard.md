# ThingsBoard LwM2M setup

This guide covers ThingsBoard CE **4.4.0 with
[PR #16179](https://github.com/thingsboard/thingsboard/pull/16179)** for native
X.509 device auto-creation, and the pre-created endpoint mode tested with
4.3.1.5. Replace example names and paths with those of your server, device and CA.

The automatic-provisioning checks used PR revision
`9a8ebae83ccebdd6eda97c22f227c3fa5e746063`. The PR was still open at validation
time; do not assume that an unpatched 4.4.0 installation includes this behavior.
Apply the matching changes to both the ThingsBoard core and LwM2M transport,
including when those services are deployed separately.

Follow the official [getting-started guide](https://thingsboard.io/docs/reference/lwm2m-api/getting-started/)
and [RPC reference](https://thingsboard.io/docs/reference/lwm2m-api/rpc-commands/)
for the installed ThingsBoard version. The RPC catalog is not a list of features
implemented by this firmware.

## Server identity and trust

Enable the secure LwM2M listener, normally UDP 5686, and make it reachable from
the device. The firmware does not use bootstrap or plaintext LwM2M. On a native
installation, the tested environment-variable configuration is:

```sh
export LWM2M_ENABLED=true
export LWM2M_ENABLED_BS=false
export LWM2M_BIND_ADDRESS=127.0.0.1
export LWM2M_SECURITY_BIND_ADDRESS=0.0.0.0
export LWM2M_SECURITY_BIND_PORT=5686
export LWM2M_SERVER_CREDENTIALS_ENABLED=true
export LWM2M_SERVER_CREDENTIALS_TYPE=PEM
export LWM2M_SERVER_PEM_CERT=/etc/thingsboard/lwm2m/server-chain.pem
export LWM2M_SERVER_PEM_KEY=/etc/thingsboard/lwm2m/server-key.pem
export LWM2M_SERVER_PEM_KEY_PASSWORD=''
export TB_LWM2M_SERVER_SECURITY_SKIP_VALIDITY_CHECK_FOR_CLIENT_CERT=false
export LWM2M_TRUST_CREDENTIALS_ENABLED=true
export LWM2M_TRUST_CREDENTIALS_TYPE=PEM
export LWM2M_TRUST_PEM_CERT=/etc/thingsboard/lwm2m/device-root-ca.pem
```

Set these through the deployment's supported configuration mechanism; exporting
them in an unrelated shell does not update an existing service. The empty key
password example assumes an unencrypted service key protected by filesystem
permissions. The service must be able to read the configured files. Limit network
exposure to the intended interface/firewall policy. Verify the actual UDP socket
after applying configuration; a running service is not proof of listener readiness.

Use an ECDSA server certificate with a DNS name matching `LWM2M_SERVER_URI`.
The configured client suites are documented in [compatibility](compatibility.md).
`root_ca.crt` on the ESP32 must trust this server's chain. That trust store is
independent of the CA that issued `device.crt`. For example, a server using a
public HTTPS certificate needs the corresponding public trust roots on the
device, even if device identities are issued by a private CA.

## Device identity

Use the same value for the device certificate CN, ThingsBoard LwM2M endpoint,
firmware `LWM2M_ENDPOINT`, and USB tool `--device-name`. For example:
`esp32-dht11-01`. The device certificate must allow TLS client authentication.
With the provisioning regex `(.+)`, the ThingsBoard device name also uses that
value. A regex that captures only part of the CN changes the device name; the
full leaf CN remains the LwM2M endpoint.

### Migration from MQTT

A CA certificate used for chain provisioning can be registered with only one
device profile. If it is already registered with an MQTT profile, disable
the old profile's provisioning strategy before configuring the LwM2M profile
with the same CA. The old profile then stops automatically creating new devices
with that CA. Plan this transfer before switching clients that rely on it.

To retain an existing device and its telemetry history, keep the device record
and explicitly configure its LwM2M credentials with X.509 security and the full
certificate CN endpoint. Assigning a LwM2M profile alone does not convert the
existing MQTT credentials into `LWM2M_CREDENTIALS`.

### Native auto-creation on 4.4.0 with PR #16179

1. Create a device profile with **Transport type: LWM2M** and the telemetry
   configuration below.
2. In **Device provisioning**, select **X509 Certificates Chain**, enter the
   profile CA certificate in PEM format, enable **Create new devices**, and set
   **CN Regular Expression variable** to `(.+)` for the example identity above.
   The first regex capture determines the ThingsBoard device name.
3. Include that profile CA in the device's transmitted certificate chain.
   A CA present only in the listener trust store does not satisfy this profile
   lookup. The USB bundle's ordered `device.crt` contains the leaf, intermediate
   certificates and root.
4. Connect using the full certificate CN as the endpoint. The patched server
   creates the device and structured `LWM2M_CREDENTIALS` with client security
   mode `X509`, the endpoint and the pinned leaf certificate. Verify registration
   and stored telemetry, then reconnect and confirm the device ID is reused.

Automatic creation, telemetry delivery and reconnection with the same device ID
and credentials were verified with Linux Anjay and ESP32 clients using new RSA
identities. This flow uses neither LwM2M bootstrap nor a separate MQTT/CoAP
provisioning request. Required bootstrap schema sections in the generated
credentials do not enable a bootstrap connection. See
[test coverage](../tests/README.md#server-automatic-provisioning-validation) for
the tested configuration and limitations.

### Pre-created endpoint mode on 4.3.1.5

Create the device, assign the LwM2M profile, and select LwM2M credentials with
X.509 security. Set the endpoint to the full certificate CN. The tested CA-trust
path verifies the device chain using the server's configured trusted CA, then
authorizes the CN against this existing endpoint. In this mode the per-device
certificate field is empty; certificate-chain authentication remains enabled.

In the 4.3.1.5 checks, the direct client-certificate field rejected an RSA
certificate while this CA-trust path accepted it. That historical limitation
does not describe the patched 4.4.0 path, which successfully stores and uses a
pinned RSA leaf. Also on 4.3.1.5, an untrusted certificate with the correct CN
and a trusted identity with an unknown endpoint were rejected; native device
auto-creation did not succeed.

When migrating an existing device, retain its record to preserve telemetry
history and use a separate profile if the old one is shared by other devices.
Do not delete an existing device merely to test provisioning. Use a separate
test identity and endpoint for that check.

## Telemetry profile

Set `profileData.transportConfiguration.observeAttr` to
[telemetry-mapping.json](../thingsboard/telemetry-mapping.json), preserving the
other profile settings. Use Temperature 3303 and Humidity 3304, both object
version 1.1, instance 0. Do not enable composite observation for this mapping.

| Resource | Purpose | Persisted key |
| --- | --- | --- |
| /3303/0/5700 | Temperature, Float, degrees Celsius | temperature |
| /3304/0/5700 | Relative humidity, Float, percent | humidity |
| /3303/0/5518 and /3304/0/5518 | Successful measurement timestamp | None |
| /3303/0/5701 and /3304/0/5701 | Units: Cel and %RH | None |

Observe the complete instances, including Timestamp 5518. Observing only Sensor
Value would suppress repeated equal readings. No maximum-period heartbeat is
configured: a failed read must not publish cached data. Before the first valid
sample the value and timestamp resources are absent; an early direct Read can
therefore return Not Found. Instance Observe can start with the units resource
and deliver values after the first sample.

After registration, inspect the device's stored telemetry and confirm
new timestamps roughly every five seconds, including unchanged values. A CoAP
acknowledgement confirms protocol receipt; check stored telemetry separately.
See [hardware validation scope](../tests/README.md#hardware-validation-scope)
for tested behavior and limitations.

## Firmware updates

The application supports ThingsBoard's native **Object 5 / Resource 0 binary
push** strategy over its existing X.509 DTLS session. Follow [OTA setup](ota.md)
for the additional observations, package version/tag values, first USB
installation and rollback behavior. Object 5 and Device Object 3 use object
version 1.0; sensor objects retain version 1.1.

Use [ota-telemetry-mapping.json](../thingsboard/ota-telemetry-mapping.json) for
the combined sensor and OTA observations. It preserves the sensor mapping
above and adds firmware version and the complete Object 5 instance. OTA
resources are not added to the sensor telemetry list. ThingsBoard's native OTA
service also emits its own `fw_state` and `lwm2m_log` status telemetry. This
operational status is separate from the two persisted sensor measurements.

Assigning a firmware package can immediately start its transfer and
installation: the reviewed 4.4.0-SNAPSHOT transport automatically executes
`/5/0/2` after receiving State = Downloaded. Schedule the assignment itself for
the intended maintenance window. The prior X.509 and sensor integration results
do not establish OTA validation on a live server or physical board.
