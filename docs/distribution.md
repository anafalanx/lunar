# Distribution guide

How Lunar gets from a working tree onto a stranger's machine without
SmartScreen scaring them off. Audience: whoever cuts releases.

## 1. Code signing (Authenticode)

Unsigned exes get the red "Windows protected your PC" interstitial and
poor winget/Defender treatment (an unsigned build here has tripped
Defender's `Wacatac` ML heuristic mid-link). Every published Lunar
binary is Authenticode-signed and RFC-3161 timestamped.

### Certum Open Source Code Signing (what we use)

Lunar is signed with a **Certum Open Source Code Signing** certificate
through **SimplySign** — the same cloud-key setup used for the author's
other tools (`els`, `drang`). No hardware dongle: the private key lives
in Certum's cloud HSM and is exposed to `signtool` through SimplySign
Desktop.

Publisher on the signature reads
`Open Source Developer Vincent Vercauteren`.

Steps to sign a release:

1. **Connect SimplySign Desktop** and log in (phone OTP). While the
   session is active, the code-signing certificate appears in
   `Cert:\CurrentUser\My` and `signtool /a` will auto-select it.
2. Sign the staged exe:

   ```
   signtool sign /a /tr http://time.certum.pl /td sha256 /fd sha256 /v Lunar.exe
   ```

   The `/tr … /td sha256` RFC-3161 timestamp is mandatory so the
   signature outlives the certificate.
3. Verify before publishing:

   ```
   signtool verify /pa /all /v Lunar.exe
   ```

   Confirm the chain terminates at `Certum Trusted Network CA` and the
   output says `The signature is timestamped:`.

> SmartScreen reputation for an OV/open-source cert accrues per-cert and
> per-file; expect a warning on the first downloads of a new cert until
> reputation builds. Keeping the same cert and always timestamping is
> what makes that reputation stick.

### The `LUNAR_SIGN_CMD` hook

`scripts/release.py` runs `LUNAR_SIGN_CMD` (with `{exe}` replaced by the
staged exe path) after the build and before hashing, then verifies with
`signtool verify /pa`. If `LUNAR_SIGN_CMD` is unset it prints a prominent
UNSIGNED warning and continues — dev dry-runs stay friction-free, real
releases must set it:

```
set LUNAR_SIGN_CMD=signtool sign /a /tr http://time.certum.pl /td sha256 /fd sha256 /v {exe}
```

## 2. Release checklist

1. **Bump `VERSION`** at the repo root (plain `X.Y.Z`).
2. **Regenerate tzdata if stale** — check IANA for a newer release than
   the embedded one (`src/tz_embed.c`) and regenerate if so.
3. **Build**: `python scripts/build.py --no-desktop`.
4. **Test**: `python tests/run_tests.py` (unit) and
   `python tests/test_smoke.py` (launches a real window; needs a desktop
   session). Both must be green — CI must also be green on the release
   commit.
5. **Sign + verify**: connect SimplySign, then sign the built
   `build/Lunar.exe` with the command in §1 and verify it. Note the
   printed SHA-256 of the signed exe.
6. **GitHub Release**: tag `vX.Y` (or `vX.Y.Z`), upload the signed
   `Lunar.exe` as the release asset, and paste its SHA-256 into the
   notes. Lunar ships as a single self-contained exe — the exe *is* the
   artifact, no installer or archive required.
7. **Round-trip**: download the published asset, re-run
   `signtool verify /pa /all /v` and compare SHA-256 against the notes.
8. **winget PR** (optional): instantiate `packaging/winget/` templates
   with the version, the signed exe's `InstallerUrl`, and its SHA-256,
   validate, and PR to microsoft/winget-pkgs (see
   `packaging/winget/README.md`). The `portable` install type points
   directly at the signed exe.

## 3. Updates and deferred scope

- **Passive update check (shipped).** Lunar notices when a newer release
  exists by querying the GitHub Releases API over its *own* hardened,
  CA-validated stack (pinned DoH + mbedTLS, no external process) and
  surfaces a notice that links to the release page. It downloads and
  installs nothing — see `src/update_check.c`.
- **Auto-download/swap (deferred).** Fetching, verifying, and swapping
  the binary in place is a security-critical subsystem (signature
  verification of the update, rollback protection) not yet built. Until
  then, users update via winget or by replacing the exe; every release's
  SHA-256 is published in the release notes and the binary is signed, so
  a replacement can be verified before it runs.
- **MSI/MSIX installer (deferred).** The single-file portable exe plus
  winget's `portable` type covers current needs; an installer only earns
  its keep once per-machine installs or auto-update exist.
