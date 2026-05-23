param(
    [switch]$Install
)

$ErrorActionPreference = 'Stop'

$projectDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Resolve-Path (Join-Path $projectDir '..\..')
$sdkRoot = 'C:\Program Files (x86)\Android\android-sdk'
$ndkRoot = 'C:\AndroidNDK-r27c'
$buildTools = Join-Path $sdkRoot 'build-tools\36.0.0'
$androidJar = Join-Path $sdkRoot 'platforms\android-35\android.jar'
$outDir = Join-Path $projectDir 'out'
$libsDir = Join-Path $outDir 'libs'
$objDir = Join-Path $outDir 'obj'
$apkBase = Join-Path $outDir 'HandSightCamera-base.apk'
$apkAligned = Join-Path $outDir 'HandSightCamera-aligned.apk'
$apkSigned = Join-Path $outDir 'HandSightCamera-signed.apk'
$keystore = Join-Path $outDir 'handsight-debug.keystore'

New-Item -ItemType Directory -Force -Path $outDir | Out-Null

$env:NDK_ROOT = $ndkRoot

$ndkBuildCmd = Join-Path $ndkRoot 'ndk-build.cmd'
& cmd /c "`"$ndkBuildCmd`" NDK_PROJECT_PATH=`"$projectDir`" APP_BUILD_SCRIPT=`"$($projectDir)\Android.mk`" NDK_APPLICATION_MK=`"$($projectDir)\Application.mk`" NDK_OUT=`"$objDir`" NDK_LIBS_OUT=`"$libsDir`""

$manifest = Join-Path $projectDir 'AndroidManifest.xml'
$aapt2 = Join-Path $buildTools 'aapt2.exe'
$zipalign = Join-Path $buildTools 'zipalign.exe'
$apksigner = Join-Path $buildTools 'apksigner.bat'
$nativeLib = Join-Path $libsDir 'arm64-v8a\libhandsight_camera.so'

if (-not (Test-Path $nativeLib)) {
    throw "Native library was not produced at $nativeLib"
}

if (Test-Path $apkBase) { Remove-Item $apkBase -Force }
if (Test-Path $apkAligned) { Remove-Item $apkAligned -Force }
if (Test-Path $apkSigned) { Remove-Item $apkSigned -Force }

& $aapt2 link `
    -I $androidJar `
    --manifest $manifest `
    --min-sdk-version 24 `
    --target-sdk-version 35 `
    --version-code 1 `
    --version-name 1.0 `
    -o $apkBase

Add-Type -AssemblyName System.IO.Compression.FileSystem
Add-Type -AssemblyName System.IO.Compression
$zip = [System.IO.Compression.ZipFile]::Open($apkBase, [System.IO.Compression.ZipArchiveMode]::Update)
try {
    $entryName = 'lib/arm64-v8a/libhandsight_camera.so'
    $existing = $zip.Entries | Where-Object { $_.FullName -eq $entryName }
    if ($existing) {
        $existing.Delete()
    }
    $entry = $zip.CreateEntry($entryName, [System.IO.Compression.CompressionLevel]::Optimal)
    $stream = $entry.Open()
    $bytes = [System.IO.File]::ReadAllBytes($nativeLib)
    $stream.Write($bytes, 0, $bytes.Length)
    $stream.Dispose()
}
finally {
    $zip.Dispose()
}

& $zipalign -f 4 $apkBase $apkAligned

if (-not (Test-Path $keystore)) {
    & keytool -genkeypair `
        -keystore $keystore `
        -storepass android `
        -keypass android `
        -alias handsight `
        -keyalg RSA `
        -keysize 2048 `
        -validity 10000 `
        -dname "CN=HandSight, OU=Dev, O=HandSight, L=Local, S=Local, C=US"
}

& $apksigner sign `
    --ks $keystore `
    --ks-pass pass:android `
    --key-pass pass:android `
    --ks-key-alias handsight `
    --out $apkSigned `
    $apkAligned

Write-Host $apkSigned

if ($Install) {
    & adb install -r $apkSigned
}
