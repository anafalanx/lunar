# Lunar Pin Enrollment

Lunar no longer ships provider cryptographic material in `Lunar.exe`.
The executable contains endpoint metadata only: host names, bootstrap IPs for DoH,
labels, ports, and operator-family names. The trust data needed for DoH and NTS is
created locally on first use.

## Trust Model

First run, renewal, and expired-pin recovery use the Windows Web PKI:

1. Lunar opens TLS 1.3 to the configured DoH resolver or NTS-KE server.
2. mbedTLS performs the TLS transport with certificate verification disabled inside mbedTLS.
3. Lunar passes the peer certificate chain to Windows certificate-chain APIs.
4. Windows builds a server-auth chain from the current machine/user certificate stores.
5. Windows applies hostname validation with `CERT_CHAIN_POLICY_SSL`.
6. Lunar hashes the validated leaf certificate's SubjectPublicKeyInfo (SPKI) with SHA-256.
7. Lunar stores that SPKI digest locally as the continuity pin for the endpoint.

After enrollment, ordinary operation is pin-first. If the local SPKI matches, the TLS
connection is accepted without needing a fresh CA decision. When a pin reaches its renewal
window, Lunar keeps accepting the still-matching pin while it attempts a fresh Windows CA
validation. If the pin has passed the certificate `notAfter` time, the pin is no longer
usable by itself; Lunar must obtain a fresh Windows CA validation or the endpoint fails
closed.

External network failures can therefore cause INOP, but they cannot force Lunar to replace
a valid local pin outside the renewal/expiry path. A pin mismatch before renewal is rejected
without CA refresh and logged as a suspicious endpoint change.

## Local Storage

Pins are stored in `%APPDATA%\Lunar\pins.dat`.

The file is plaintext only inside the process. On disk it is protected with Windows DPAPI
via `CryptProtectData` / `CryptUnprotectData`, using the current user's Windows profile as
the cryptographic protection boundary. Lunar writes through a temporary file, flushes it,
and atomically replaces the old cache with `MoveFileExW(..., MOVEFILE_WRITE_THROUGH)`.

After each write, Lunar applies a protected DACL that grants access to the current user and
SYSTEM. This blocks accidental edits and ordinary cross-user tampering.

Important limitation: Windows DPAPI cannot prove that only this exact executable is reading
the file. Code already running as the same Windows user can usually call the same DPAPI
unprotect operation. Lunar's local storage protects against accidental edits, casual local
tampering, and other users on the same machine; it is not a same-user arbitrary-code
execution boundary.

## Renewal Policy

Lunar stores certificate validity metadata next to each SPKI digest:

- endpoint kind: DoH or NTS
- endpoint label, host, port, and operator family
- SPKI SHA-256 digest and hex form
- certificate `notBefore` and `notAfter`
- Unix timestamps for validity and computed renewal time
- last enrollment status

The renewal window starts 30 days before `notAfter`. During that window, a matching pin can
continue to authenticate the endpoint while CA renewal is attempted. After `notAfter`, CA
validation is mandatory.

If CA validation succeeds, Lunar saves the observed leaf SPKI with one of these statuses:

- `first-run-enrollment`
- `scheduled-renewal`
- `expired-renewal`
- `pin-rotation`

## NTS Concurrence

Because first-run trust now depends on maintained public CA infrastructure, Lunar does not
allow a single NTS source to anchor the clock. A trusted cycle requires both NTS slots to
succeed, both to be authenticated by enrolled pins, and the two NTS providers to come from
different operator families. They must agree within 200 ms, and at least three of the four
plain SNTP core sources must agree with their midpoint.

If fewer than two operator-diverse NTS samples are available, Lunar stays INOP.

## Logging

Lunar logs the enrollment path in detail:

- missing, expired, matching, mismatching, and renewal-due local pin states
- Windows CA validation attempts and outcome
- certificate subject, issuer, `notBefore`, `notAfter`, SPKI digest, chain status, policy status, and revocation status
- DPAPI decrypt/protect failures
- pin-cache parse/schema failures
- atomic cache writes and endpoint save status
- NTP cycle gates, including the two-operator NTS requirement

Revocation checking is requested. If Windows reports only offline or unknown revocation
status, Lunar records that fact and retries the chain build without revocation so that a
temporary responder outage does not permanently brick first-run enrollment. Hard chain or
hostname policy errors still fail the enrollment.