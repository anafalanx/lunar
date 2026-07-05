# winget manifest templates for Lunar

These are **templates**, not submittable manifests. They cannot be
submitted to [microsoft/winget-pkgs](https://github.com/microsoft/winget-pkgs)
until real GitHub Releases exist, because winget requires:

1. **A stable, public download URL.** The `InstallerUrl` must point at an
   immutable asset — a GitHub Release download URL
   (`.../releases/download/v<version>/Lunar.exe`) is the intended host.
   Draft releases, CI artifacts, and branch-relative URLs are rejected.
2. **A signed executable.** Lunar ships as a single portable exe; it must
   carry a valid Authenticode signature. Unsigned binaries trip
   Defender/SmartScreen heuristics and are likely to fail winget-pkgs
   validation labs. See `docs/distribution.md` for signing setup.
3. **A hash that matches.** `InstallerSha256` is the SHA-256 of the
   uploaded **Lunar.exe**.

## Generating real manifests from these templates

For version `X.Y` with release asset `Lunar.exe`:

1. Compute the exe hash:
   `certutil -hashfile dist/Lunar.exe SHA256`
2. In all three `Lunar.Lunar.*.yaml` files, replace:
   - `{VERSION}` -> `X.Y`
   - `{SHA256}`  -> the exe hash (uppercase or lowercase both accepted)
3. Copy the three files into a winget-pkgs fork under
   `manifests/l/Lunar/Lunar/X.Y/`.
4. Validate locally: `winget validate manifests/l/Lunar/Lunar/X.Y` and
   test-install with
   `winget install --manifest manifests/l/Lunar/Lunar/X.Y`
   (requires enabling `LocalManifestFiles` in winget settings).
5. Open a PR against microsoft/winget-pkgs. Subsequent versions can use
   `wingetcreate update Lunar.Lunar -u <new exe url> -v X.Y` instead of
   hand-editing.

## Files

| File                            | ManifestType  |
| ------------------------------- | ------------- |
| `Lunar.Lunar.yaml`              | version       |
| `Lunar.Lunar.installer.yaml`    | installer     |
| `Lunar.Lunar.locale.en-US.yaml` | defaultLocale |
