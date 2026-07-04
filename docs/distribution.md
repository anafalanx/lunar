# Distribution guide

How Lunar gets from a working tree onto a stranger's machine without
SmartScreen scaring them off. Audience: whoever cuts releases.

## 1. Code signing (Authenticode)

Unsigned exes get the red "Windows protected your PC" interstitial and
poor winget/Defender treatment. Two realistic options for an indie/OSS
Windows product in 2026:

### Option A (recommended): Azure Trusted Signing

- ~$9.99/month (Basic tier), no hardware token, no cert files to guard.
- Certs are short-lived and auto-rotated; identity validation is done
  once against you/your org.
- Integrates directly with `signtool` via a dlib, so it slots into
  `LUNAR_SIGN_CMD` unchanged.
- Microsoft-operated, so SmartScreen reputation accrues noticeably
  faster than with third-party OV certs.

Setup once: create a Trusted Signing account + certificate profile in
Azure, install the client tools (`Azure.CodeSigning.Dlib.dll`, comes with
the "Trusted Signing Client Tools" / `Microsoft.Trusted.Signing.Client`
NuGet package), write `metadata.json` with your `Endpoint`,
`CodeSigningAccountName`, `CertificateProfileName`, and authenticate
(e.g. `az login` or the `AZURE_TENANT_ID`/`AZURE_CLIENT_ID`/
`AZURE_CLIENT_SECRET` env vars).

Then:

```
set LUNAR_SIGN_CMD=signtool sign /v /fd SHA256 /tr http://timestamp.acs.microsoft.com /td SHA256 /dlib "C:\tools\Azure.CodeSigning.Dlib.dll" /dmdf "C:\tools\metadata.json" {exe}
```

### Option B: classic OV code-signing certificate

- ~$200-400/year from Certum, Sectigo, SSL.com, etc.
- Since the 2023 CA/B rules, the private key must live on a FIPS token
  or cloud HSM — expect a USB dongle (blocks headless CI) or the CA's
  cloud-signing service.
- SmartScreen reputation builds slowly per-cert; expect warnings on the
  first weeks/downloads of each new cert.

```
set LUNAR_SIGN_CMD=signtool sign /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 /sha1 <CERT_THUMBPRINT> {exe}
```

### How the hook works

`scripts/release.py` runs `LUNAR_SIGN_CMD` (with `{exe}` replaced by the
staged exe path) after the build and before hashing/zipping, then
verifies with `signtool verify /pa` if signtool is on PATH. If
`LUNAR_SIGN_CMD` is unset it prints a prominent UNSIGNED warning and
continues — dev dry-runs stay friction-free, real releases must set it.
Always use an RFC 3161 timestamp (`/tr ... /td SHA256`) so signatures
outlive the cert.

## 2. Release checklist

1. **Bump `VERSION`** at the repo root (plain `X.Y.Z`).
2. **Regenerate tzdata if stale** — check IANA for a newer release than
   the embedded one (`src/tz_embed.c`) and regenerate if so.
3. **Build**: `python scripts/build.py --no-desktop`.
4. **Test**: `python tests/run_tests.py` (unit) and
   `python tests/test_smoke.py` (launches a real window; needs a desktop
   session). Both must be green — CI must also be green on the release
   commit.
5. **Sign + zip**: set `LUNAR_SIGN_CMD` (above), then
   `python scripts/release.py`. Confirm the log shows "Signature
   verified" and "Copied LICENSE", and note the printed SHA-256.
6. **GitHub Release**: tag `vX.Y.Z`, upload
   `dist/Lunar-X.Y.Z-win-x64.zip`, paste the exe SHA-256 into the notes.
7. **winget PR**: instantiate `packaging/winget/` templates with the
   version and the **zip's** SHA-256, validate, and PR to
   microsoft/winget-pkgs (see `packaging/winget/README.md`).

## 3. Deliberately deferred to the Rust rewrite

- **Auto-update client.** Update checking/downloading/swapping is a
  security-critical subsystem (TLS pinning, signature verification of
  updates, rollback protection) that we do not want to build twice in C.
  Until then: users update via winget or by replacing the exe; each
  release's SHA-256 is published in README.txt and the release notes.
- **MSI/MSIX installer.** The single-file portable exe plus winget's
  `portable` type covers current needs; an installer only earns its keep
  once auto-update or per-machine installs exist.
