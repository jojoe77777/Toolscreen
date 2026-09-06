# Repack the released Toolscreen 1.4.4 DLL

This workflow builds a new x64 installer around the exact released, signed
Toolscreen 1.4.4 and liblogger 1.0.2 DLLs. It does not compile native Toolscreen
code, fetch a newer logger, require PDBs, or alter the ordinary universal build.

## Run in GitHub Actions

Run **Build Signed Release** on the branch containing this change:

- `repack_144`: **true**
- `installer_version`: **1.4.5** (or the new three-component installer version)
- `repack_sign`: **true**

The existing workflow calls `signed-repack.yml` from the same commit. This also
allows a review-branch run before the new standalone workflow is on the default
branch. Once merged, **Repack and Sign Toolscreen 1.4.4** can be dispatched directly.
Set the signing input to false for an unsigned CI rehearsal.

Download `toolscreen-1.4.5-x64-repack-signed` after a successful signing run.
It contains the signed JAR/EXE, `provenance.json`, and `SHA256SUMS.txt`.
The run does not publish or replace a GitHub release. Describe the installer-only
change and x64 payload support when publishing the new release. The upstream
automatic-update endpoint remains enabled and follows the latest Toolscreen
release; this is not a permanently frozen 1.4.4 update channel.

## Inputs and source changes

`payload.json` pins the original release download, archive and DLL hashes,
payload versions, and EasyInjectBundled commit. `prepare.ps1` checks the archive,
PE architecture, versions, and Authenticode signatures. It applies the committed
`easyinject-x64.patch` to a clean dependency checkout and stages only the two
specified payloads. Run it only against a disposable build checkout: it removes
existing files from that checkout's two DLL input directories.

The patch adds an opt-in x64 packaging mode to the compatibility Maven module
and native installer, enables JUnit 5 tests through Surefire, and makes the JAR
reject a missing architecture before modifying an instance. The upstream default
remains universal. The repack builds only the compatibility variant; it does not
ship the reduced variant or the generic downloader. An x64 Java runtime is
required for this JAR's native payload.

The wrapper CMake project supplies installer version metadata independently of
the old DLL version. It intentionally avoids the main project's version checker,
which expects payload and installer versions to match. `verify.ps1` checks both
versions and extracts the JAR and EXE resources without running the installer.
Every embedded native payload must match its pinned hash before and after signing.

## SignPath configuration

The workflow uses the existing `Toolscreen` project, `release-signing` policy,
and `jar` / `installer` artifact configurations on GitHub-hosted runners. Uploads
are submitted using their GitHub artifact IDs, preserving origin verification.

The `version` parameter is the **installer** version. If a server-side artifact
configuration currently requires both architectures, matches an old DLL resource
name, requires the embedded DLL version to equal `${version}`, or re-signs nested
DLLs, it must be adapted for this package. Preserve/verify the existing DLL
signatures rather than changing the payload bytes. Keep origin verification and
normal signing approval requirements enabled. The post-sign hash checks fail if
SignPath changes either embedded DLL. Server-side settings are not stored here.

## Local validation

With a fresh checkout of the pinned EasyInjectBundled commit at `out/repack/source`,
Java/Maven, CMake, and Visual Studio 2022 installed, run from the Toolscreen root:

```powershell
./scripts/repack/prepare.ps1 -SourceDirectory out/repack/source -DownloadDirectory out/repack/downloads
mvn -f out/repack/source/pom.xml -pl compatibility-variant '-Deasyinject.x64Only=true' --batch-mode clean package
$source = (Resolve-Path out/repack/source).Path
cmake -S scripts/repack -B out/repack/build -G 'Visual Studio 17 2022' -A x64 "-DEASYINJECT_SOURCE_DIR=$source" '-DINSTALLER_VERSION=1.4.5'
cmake --build out/repack/build --config Release --parallel --target EasyInjectExe easyinject_instance_cfg_tests easyinject_launcher_parity_tests easyinject_installer_io_tests
ctest --test-dir out/repack/build -C Release --output-on-failure -R '^(instance_cfg_sections|launcher_parity|installer_io)$'
New-Item -ItemType Directory -Force out/repack/artifacts
Copy-Item out/repack/source/target/Toolscreen-1.4.5-double-click-me.jar out/repack/artifacts
Copy-Item out/repack/build/easyinject/Release/Toolscreen-1.4.5-double-click-me.exe out/repack/artifacts
./scripts/repack/verify.ps1 -ArtifactDirectory out/repack/artifacts
```

Use `-RequireSigned` to verify the final SignPath outputs. Local packages are
unsigned installers even though their embedded DLLs retain their valid signatures.
