# Signed Toolscreen 1.4.4 payload repack

Run **Build Signed Release** from the branch containing this workflow with:

- `repack_144`: true
- `installer_version`: 1.4.7
- `repack_sign`: true

The dedicated workflow builds new installers using the EasyInjectBundled commit
pinned in `payload.json`, currently `4c93d4e63c3d8c4813bcaa68ebedec60dc555833`.
This revision already packages x64 payloads, so no source patch is applied.
Only branding and the staged DLL inputs change in the dependency checkout;
the wrapper adds installer PE version metadata.

The embedded Toolscreen DLL stays byte-identical to the signed 1.4.4 release,
and liblogger stays at the original signed 1.0.2 version. `prepare.ps1` verifies
the release archive and DLL hashes, architectures, versions, and signatures.
Run preparation only against a disposable dependency checkout: it clears files
from that checkout's two DLL input directories before staging the pinned inputs.

The selected upstream revision no longer includes the previous variant modules
or Java/native test suites. CI builds its root Maven project and native EXE.
`verify.ps1` checks both packages' exact payload bytes and installer version
before signing, then verifies signatures and unchanged payload bytes afterward.
It reads EXE resources as data without running the installer.

Approve the fresh JAR and then EXE requests in SignPath. The existing `jar` and
`installer` configurations and `release-signing` policy are used, preserving
GitHub origin verification. The `version` parameter is the installer version.
Existing embedded DLL signatures must be preserved, not replaced.

Download `toolscreen-1.4.7-x64-repack-signed` from the successful run. It includes
both signed installers, `provenance.json`, and `SHA256SUMS.txt`. No GitHub release
is published automatically. The existing updater endpoint remains enabled.

For local validation, build the dependency root with `mvn clean package`, and
configure `scripts/repack` with `EASYINJECT_SOURCE_DIR` and `INSTALLER_VERSION`.
Build target `EasyInjectExe`. Stage both artifacts in one directory, then run
`verify.ps1 -ArtifactDirectory <directory> -InstallerVersion 1.4.7`.
Add `-RequireSigned` when checking downloaded SignPath outputs.
