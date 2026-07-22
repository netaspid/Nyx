# One-shot: install deps (Windows), configure Qt/Android, build and sign a sideloadable APK.
# Run from PowerShell:  .\scripts\build-android-apk.ps1
# Optional:             .\scripts\build-android-apk.ps1 -DepsOnly

[CmdletBinding()]
param(
    [switch]$DepsOnly
)

$ErrorActionPreference = "Stop"

function Log([string]$msg) { Write-Host "[nyx-android] $msg" }
function Die([string]$msg) { Write-Error "[nyx-android] ERROR: $msg"; exit 1 }

$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$BuildDir = if ($env:NYX_ANDROID_BUILD_DIR) { $env:NYX_ANDROID_BUILD_DIR } else { Join-Path $Root "build-android" }
$QtVer = if ($env:NYX_QT_VERSION) { $env:NYX_QT_VERSION } else { "6.5.3" }
$QtRoot = if ($env:NYX_QT_ROOT) { $env:NYX_QT_ROOT } else { Join-Path $env:LOCALAPPDATA "Qt" }
$SdkRoot = if ($env:ANDROID_SDK_ROOT) { $env:ANDROID_SDK_ROOT } else { Join-Path $env:LOCALAPPDATA "Android\Sdk" }
$NdkVersion = if ($env:NYX_ANDROID_NDK_VERSION) { $env:NYX_ANDROID_NDK_VERSION } else { "25.2.9519653" }
$ApiLevel = if ($env:NYX_ANDROID_API) { $env:NYX_ANDROID_API } else { "34" }
$BuildTools = if ($env:NYX_ANDROID_BUILD_TOOLS) { $env:NYX_ANDROID_BUILD_TOOLS } else { "34.0.0" }
$OutApk = Join-Path $BuildDir "Nyx.apk"
$Keystore = if ($env:NYX_ANDROID_KEYSTORE) { $env:NYX_ANDROID_KEYSTORE } else { Join-Path $env:USERPROFILE ".android\debug.keystore" }
$KeyAlias = if ($env:NYX_ANDROID_KEY_ALIAS) { $env:NYX_ANDROID_KEY_ALIAS } else { "androiddebugkey" }
$StorePass = if ($env:NYX_ANDROID_STOREPASS) { $env:NYX_ANDROID_STOREPASS } else { "android" }
$KeyPass = if ($env:NYX_ANDROID_KEYPASS) { $env:NYX_ANDROID_KEYPASS } else { "android" }

function Ensure-WingetPackage([string]$Id) {
    $found = winget list --id $Id -e 2>$null | Select-String -Pattern $Id
    if (-not $found) {
        Log "Installing $Id via winget…"
        winget install --id $Id -e --accept-package-agreements --accept-source-agreements
    }
}

function Ensure-Tools {
    if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
        Die "winget is required (Windows Package Manager)."
    }
    Ensure-WingetPackage "Kitware.CMake"
    Ensure-WingetPackage "Ninja-build.Ninja"
    Ensure-WingetPackage "Microsoft.OpenJDK.17"
    Ensure-WingetPackage "Python.Python.3.12"
    # Perl for libaom
    if (-not (Get-Command perl -ErrorAction SilentlyContinue)) {
        Ensure-WingetPackage "StrawberryPerl.StrawberryPerl"
    }
}

function Pick-Java17 {
    $candidates = @(
        "${env:ProgramFiles}\Microsoft\jdk-17*\bin\java.exe",
        "${env:ProgramFiles}\Eclipse Adoptium\jdk-17*\bin\java.exe",
        "${env:ProgramFiles}\Java\jdk-17*\bin\java.exe"
    )
    $java = $null
    foreach ($pattern in $candidates) {
        $hit = Get-Item $pattern -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($hit) { $java = $hit.FullName; break }
    }
    if (-not $java) {
        $java = (Get-Command java -ErrorAction SilentlyContinue).Source
    }
    if (-not $java) { Die "JDK 17 not found. Install Microsoft.OpenJDK.17." }
    $env:JAVA_HOME = (Resolve-Path (Join-Path (Split-Path $java) "..")).Path
    $env:Path = "$($env:JAVA_HOME)\bin;" + $env:Path
    $ver = & java -version 2>&1 | Out-String
    if ($ver -notmatch 'version "17') {
        Die "Need JDK 17 for Gradle. Current JAVA_HOME=$($env:JAVA_HOME)"
    }
    Log "JAVA_HOME=$($env:JAVA_HOME)"
}

function Ensure-Sdk {
    $env:ANDROID_SDK_ROOT = $SdkRoot
    $env:ANDROID_HOME = $SdkRoot
    New-Item -ItemType Directory -Force -Path (Join-Path $SdkRoot "cmdline-tools") | Out-Null
    $sdkmanager = Join-Path $SdkRoot "cmdline-tools\latest\bin\sdkmanager.bat"
    if (-not (Test-Path $sdkmanager)) {
        Log "Downloading Android cmdline-tools…"
        $zip = Join-Path $env:TEMP "nyx-android-cmdline-tools.zip"
        Invoke-WebRequest -Uri "https://dl.google.com/android/repository/commandlinetools-win-11076708_latest.zip" -OutFile $zip
        $extract = Join-Path $env:TEMP "nyx-cmdline-tools"
        if (Test-Path $extract) { Remove-Item -Recurse -Force $extract }
        Expand-Archive -Path $zip -DestinationPath $extract
        $latest = Join-Path $SdkRoot "cmdline-tools\latest"
        if (Test-Path $latest) { Remove-Item -Recurse -Force $latest }
        Move-Item (Join-Path $extract "cmdline-tools") $latest
        $sdkmanager = Join-Path $SdkRoot "cmdline-tools\latest\bin\sdkmanager.bat"
    }
    $packages = @(
        "platform-tools",
        "platforms;android-$ApiLevel",
        "build-tools;$BuildTools",
        "ndk;$NdkVersion"
    )
    Log "sdkmanager: $($packages -join ', ')"
    $inputYes = "y`n" * 40
    $inputYes | & $sdkmanager --sdk_root=$SdkRoot @packages | Out-Null
    $env:ANDROID_NDK_ROOT = Join-Path $SdkRoot "ndk\$NdkVersion"
    if (-not (Test-Path $env:ANDROID_NDK_ROOT)) { Die "NDK missing at $($env:ANDROID_NDK_ROOT)" }
    Log "ANDROID_SDK_ROOT=$($env:ANDROID_SDK_ROOT)"
    Log "ANDROID_NDK_ROOT=$($env:ANDROID_NDK_ROOT)"
}

function Ensure-Aqt {
    if (Get-Command aqt -ErrorAction SilentlyContinue) { return }
    Log "Installing aqtinstall…"
    python -m pip install --user aqtinstall
    $userScripts = Join-Path $env:APPDATA "Python\Python*\Scripts"
    $aqt = Get-ChildItem $userScripts -Filter aqt.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($aqt) { $env:Path = "$($aqt.DirectoryName);" + $env:Path }
    if (-not (Get-Command aqt -ErrorAction SilentlyContinue)) {
        Die "aqt not on PATH after pip install"
    }
}

function Ensure-Qt {
    $hostArch = "win64_mingw"
    $hostDir = Join-Path $QtRoot "$QtVer\$hostArch"
    $andrDir = Join-Path $QtRoot "$QtVer\android_arm64_v8a"
    New-Item -ItemType Directory -Force -Path $QtRoot | Out-Null
    if (-not (Test-Path (Join-Path $hostDir "bin\qmake.exe"))) {
        Log "Fetching Qt $QtVer host ($hostArch)…"
        aqt install-qt windows desktop $QtVer $hostArch -O $QtRoot -m qtmultimedia
    }
    if (-not (Test-Path (Join-Path $andrDir "bin\qt-cmake.bat"))) {
        Log "Fetching Qt $QtVer Android…"
        aqt install-qt windows android $QtVer android_arm64_v8a -O $QtRoot -m qtmultimedia --autodesktop
    }
    $env:QT_HOST_PATH = $hostDir
    $script:QtAndroid = $andrDir
    Log "Qt Android: $script:QtAndroid"
    Log "Qt host:    $($env:QT_HOST_PATH)"
}

function Ensure-DebugKeystore {
    $dir = Split-Path $Keystore -Parent
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    if (-not (Test-Path $Keystore)) {
        Log "Creating debug keystore at $Keystore…"
        & keytool -genkeypair -v `
            -keystore $Keystore `
            -storepass $StorePass `
            -keypass $KeyPass `
            -alias $KeyAlias `
            -keyalg RSA -keysize 2048 -validity 10000 `
            -dname "CN=Android Debug,O=Android,C=US"
    }
}

function Configure-And-Build {
    $qtCmake = Join-Path $script:QtAndroid "bin\qt-cmake.bat"
    if (-not (Test-Path $qtCmake)) { Die "qt-cmake.bat not found: $qtCmake" }
    Log "Configuring $BuildDir…"
    & $qtCmake -B $BuildDir -G Ninja `
        "-DANDROID_SDK_ROOT=$($env:ANDROID_SDK_ROOT)" `
        "-DANDROID_NDK_ROOT=$($env:ANDROID_NDK_ROOT)" `
        "-DCMAKE_BUILD_TYPE=Release" `
        "-DQT_HOST_PATH=$($env:QT_HOST_PATH)"
    Log "Building…"
    cmake --build $BuildDir --parallel
}

function Sign-Apk {
    Ensure-DebugKeystore
    $deploySettings = Join-Path $BuildDir "android-nyx-app-deployment-settings.json"
    if (-not (Test-Path $deploySettings)) { Die "missing $deploySettings" }
    $androiddeployqt = Join-Path $env:QT_HOST_PATH "bin\androiddeployqt.exe"
    if (-not (Test-Path $androiddeployqt)) { Die "androiddeployqt.exe not found" }
    $androidBuild = Join-Path $BuildDir "android-build"
    Log "Packaging and signing APK…"
    & $androiddeployqt `
        --input $deploySettings `
        --output $androidBuild `
        --apk $OutApk `
        --release `
        --sign $Keystore $KeyAlias `
        --storepass $StorePass `
        --keypass $KeyPass
    if (-not (Test-Path $OutApk)) { Die "APK was not produced at $OutApk" }
    Log "Done: $OutApk"
    Log "Copy to the phone, allow install from this source, open the APK."
}

Ensure-Tools
Pick-Java17
Ensure-Sdk
Ensure-Aqt
Ensure-Qt
if ($DepsOnly) {
    Ensure-DebugKeystore
    Log "Dependencies ready."
    exit 0
}
Configure-And-Build
Sign-Apk
