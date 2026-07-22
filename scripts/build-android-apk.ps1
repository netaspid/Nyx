# Fully automated Windows APK builder: installs deps, Qt, SDK/NDK, builds and signs Nyx.apk.
# Usage:  .\scripts\build-android-apk.ps1
#         .\scripts\build-android-apk.ps1 -DepsOnly
#         .\scripts\build-android-apk.ps1 -InstallApk

[CmdletBinding()]
param(
    [switch]$DepsOnly,
    [switch]$InstallApk
)

$ErrorActionPreference = "Stop"
# java/keytool/sdkmanager write to stderr; do not treat that as a terminating error (PS 7+).
if ($null -ne (Get-Variable PSNativeCommandUseErrorActionPreference -ErrorAction SilentlyContinue)) {
    $PSNativeCommandUseErrorActionPreference = $false
}

function Log([string]$msg) { Write-Host "[nyx-android] $msg" }
function Die([string]$msg) {
    Write-Host "[nyx-android] ERROR: $msg" -ForegroundColor Red
    exit 1
}

function Refresh-Path {
    $machine = [Environment]::GetEnvironmentVariable("Path", "Machine")
    $user = [Environment]::GetEnvironmentVariable("Path", "User")
    $env:Path = "$machine;$user"
}

function Invoke-Quiet {
    param([scriptblock]$Block)
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        & $Block 2>&1 | ForEach-Object { $_ }
    } finally {
        $ErrorActionPreference = $prev
    }
}

function Get-NativeOutput([string]$FilePath, [string[]]$ArgumentList = @()) {
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $out = & $FilePath @ArgumentList 2>&1 | ForEach-Object { "$_" }
        return ($out -join "`n")
    } finally {
        $ErrorActionPreference = $prev
    }
}

function Find-FirstFile([string[]]$Patterns) {
    foreach ($pattern in $Patterns) {
        $hit = Get-Item $pattern -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }
    return $null
}

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
$CmdlineToolsZip = "https://dl.google.com/android/repository/commandlinetools-win-11076708_latest.zip"

function Ensure-WingetPackage([string]$Id) {
    Refresh-Path
    $list = Get-NativeOutput "winget" @("list", "--id", $Id, "-e")
    if ($list -match [regex]::Escape($Id)) {
        Log "Already installed: $Id"
        return
    }
    Log "Installing $Id via winget…"
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        & winget install --id $Id -e --accept-package-agreements --accept-source-agreements --disable-interactivity
        if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne -1978335189) {
            # -1978335189 = already installed (some winget versions)
            Die "winget failed to install $Id (exit $LASTEXITCODE)"
        }
    } finally {
        $ErrorActionPreference = $prev
    }
    Refresh-Path
}

function Ensure-Tools {
    if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
        Die "winget is required (Windows Package Manager)."
    }
    Ensure-WingetPackage "Kitware.CMake"
    Ensure-WingetPackage "Ninja-build.Ninja"
    Ensure-WingetPackage "Microsoft.OpenJDK.17"
    Ensure-WingetPackage "Python.Python.3.12"
    Ensure-WingetPackage "Google.PlatformTools"
    Ensure-WingetPackage "StrawberryPerl.StrawberryPerl"

    # Pin common install dirs onto PATH for this session.
    $extra = @(
        "${env:ProgramFiles}\CMake\bin",
        "${env:ProgramFiles}\Microsoft\jdk-17*\bin",
        "${env:LocalAppData}\Programs\Python\Python312",
        "${env:LocalAppData}\Programs\Python\Python312\Scripts",
        "${env:AppData}\Python\Python312\Scripts",
        "C:\Strawberry\perl\bin",
        "${env:LocalAppData}\Microsoft\WinGet\Links",
        "${env:LocalAppData}\Microsoft\WinGet\Packages\Google.PlatformTools*\platform-tools",
        "${env:LocalAppData}\Android\Sdk\platform-tools"
    )
    foreach ($pattern in $extra) {
        Get-Item $pattern -ErrorAction SilentlyContinue | ForEach-Object {
            $env:Path = "$($_.FullName);$env:Path"
        }
    }
    Refresh-Path
}

function Pick-Java17 {
    Refresh-Path
    $java = Find-FirstFile @(
        "${env:ProgramFiles}\Microsoft\jdk-17*\bin\java.exe",
        "${env:ProgramFiles}\Eclipse Adoptium\jdk-17*\bin\java.exe",
        "${env:ProgramFiles}\Java\jdk-17*\bin\java.exe",
        "${env:LocalAppData}\Programs\Eclipse Adoptium\jdk-17*\bin\java.exe"
    )
    if (-not $java) {
        $cmd = Get-Command java -ErrorAction SilentlyContinue
        if ($cmd) { $java = $cmd.Source }
    }
    if (-not $java) { Die "JDK 17 not found after winget install." }

    $env:JAVA_HOME = (Resolve-Path (Join-Path (Split-Path $java) "..")).Path
    $env:Path = "$($env:JAVA_HOME)\bin;$env:Path"

    $ver = Get-NativeOutput $java @("-version")
    if ($ver -notmatch 'version "17') {
        Die "Need JDK 17 for Gradle. Got:`n$ver`nJAVA_HOME=$($env:JAVA_HOME)"
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
        $extract = Join-Path $env:TEMP "nyx-cmdline-tools"
        Invoke-WebRequest -Uri $CmdlineToolsZip -OutFile $zip
        if (Test-Path $extract) { Remove-Item -Recurse -Force $extract }
        Expand-Archive -Path $zip -DestinationPath $extract -Force
        $latest = Join-Path $SdkRoot "cmdline-tools\latest"
        if (Test-Path $latest) { Remove-Item -Recurse -Force $latest }
        $src = Join-Path $extract "cmdline-tools"
        if (-not (Test-Path $src)) {
            # Some zips nest differently
            $src = Get-ChildItem $extract -Directory | Select-Object -First 1 -ExpandProperty FullName
        }
        New-Item -ItemType Directory -Force -Path (Split-Path $latest) | Out-Null
        Move-Item $src $latest
        $sdkmanager = Join-Path $SdkRoot "cmdline-tools\latest\bin\sdkmanager.bat"
    }
    if (-not (Test-Path $sdkmanager)) { Die "sdkmanager.bat missing at $sdkmanager" }

    $packages = @(
        "platform-tools",
        "platforms;android-$ApiLevel",
        "build-tools;$BuildTools",
        "ndk;$NdkVersion"
    )
    Log "Accepting Android SDK licenses…"
    $yes = ("y`n" * 80)
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $yes | & $sdkmanager --sdk_root=$SdkRoot --licenses | Out-Null
        Log "sdkmanager install: $($packages -join ', ')"
        $yes | & $sdkmanager --sdk_root=$SdkRoot --install @packages | Out-Host
        if ($LASTEXITCODE -ne 0) {
            Die "sdkmanager install failed (exit $LASTEXITCODE)"
        }
    } finally {
        $ErrorActionPreference = $prev
    }

    $env:ANDROID_NDK_ROOT = Join-Path $SdkRoot "ndk\$NdkVersion"
    if (-not (Test-Path $env:ANDROID_NDK_ROOT)) { Die "NDK missing at $($env:ANDROID_NDK_ROOT)" }
    $env:Path = "$(Join-Path $SdkRoot 'platform-tools');$env:Path"
    Log "ANDROID_SDK_ROOT=$($env:ANDROID_SDK_ROOT)"
    Log "ANDROID_NDK_ROOT=$($env:ANDROID_NDK_ROOT)"
}

function Ensure-Python {
    Refresh-Path
    $py = Find-FirstFile @(
        "${env:LocalAppData}\Programs\Python\Python312\python.exe",
        "${env:LocalAppData}\Programs\Python\Python313\python.exe",
        "${env:ProgramFiles}\Python312\python.exe"
    )
    if (-not $py) {
        $cmd = Get-Command python -ErrorAction SilentlyContinue
        if ($cmd) { $py = $cmd.Source }
    }
    if (-not $py) { Die "Python not found after winget install." }
    $env:Path = "$(Split-Path $py);$(Join-Path (Split-Path $py) 'Scripts');$env:Path"
    Log "Python: $py"
    return $py
}

function Ensure-Aqt([string]$PythonExe) {
    Log "Ensuring aqtinstall…"
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        & $PythonExe -m pip install --user --upgrade pip aqtinstall | Out-Host
        if ($LASTEXITCODE -ne 0) { Die "pip install aqtinstall failed" }
    } finally {
        $ErrorActionPreference = $prev
    }

    $aqt = Find-FirstFile @(
        "${env:AppData}\Python\Python312\Scripts\aqt.exe",
        "${env:AppData}\Python\Python313\Scripts\aqt.exe",
        "${env:LocalAppData}\Programs\Python\Python312\Scripts\aqt.exe",
        "${env:LocalAppData}\Programs\Python\Python313\Scripts\aqt.exe"
    )
    if ($aqt) {
        $env:Path = "$(Split-Path $aqt);$env:Path"
    }
    if (-not (Get-Command aqt -ErrorAction SilentlyContinue)) {
        # Fallback: python -m aqt
        $script:AqtCmd = @($PythonExe, "-m", "aqt")
    } else {
        $script:AqtCmd = @((Get-Command aqt).Source)
    }
    Log "aqt: $($script:AqtCmd -join ' ')"
}

function Write-AqtConfig {
    $cfg = Join-Path $env:TEMP "nyx-aqt-settings.ini"
    # UTF-8 without BOM; ignore_hash in ini so Windows aqt workers pick it up.
    $content = @"
[aqt]
concurrency = 2
baseurl = https://ftp.jaist.ac.jp/pub/qtproject

[requests]
connection_timeout = 30
response_timeout = 120
max_retries_to_retrieve_hash = 3
INSECURE_NOT_FOR_PRODUCTION_ignore_hash = True

[mirrors]
trusted_mirrors =
    https://ftp.jaist.ac.jp/pub/qtproject
    https://mirrors.ukfast.co.uk/sites/qt.io
    https://mirrors.ocf.berkeley.edu/qt
    https://ftp.fau.de/qtproject
    https://mirrors.dotsrc.org/qtproject
    https://mirrors.aliyun.com/qt
    https://mirrors.ustc.edu.cn/qtproject
    https://qt.mirror.constant.com
fallbacks =
    https://ftp.jaist.ac.jp/pub/qtproject
    https://mirrors.ukfast.co.uk/sites/qt.io
    https://mirrors.ocf.berkeley.edu/qt
    https://ftp.fau.de/qtproject
    https://mirrors.dotsrc.org/qtproject
    https://mirrors.aliyun.com/qt
    https://mirrors.ustc.edu.cn/qtproject
    https://qt.mirror.constant.com
    https://ftp2.nluug.nl/languages/qt
"@
    $utf8 = New-Object System.Text.UTF8Encoding $false
    [System.IO.File]::WriteAllText($cfg, $content, $utf8)
    $script:AqtConfig = $cfg
    Log "aqt config: $cfg"
}

function Invoke-Aqt([string[]]$AqtArgs) {
    $exe = $script:AqtCmd[0]
    $prefix = @()
    if ($script:AqtCmd.Count -gt 1) {
        $prefix = $script:AqtCmd[1..($script:AqtCmd.Count - 1)]
    }
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        $lines = & $exe @($prefix + $AqtArgs) 2>&1
        $code = $LASTEXITCODE
        foreach ($line in $lines) { Write-Host "$line" }
        $script:LastAqtOutput = ($lines | ForEach-Object { "$_" }) -join "`n"
        return ($code -eq 0)
    } finally {
        $ErrorActionPreference = $prev
    }
}

function Invoke-AqtInstallQt {
    param(
        [Parameter(Mandatory = $true)][string[]]$CoreArgs,
        [string]$What
    )
    $mirrors = @(
        "https://ftp.jaist.ac.jp/pub/qtproject",
        "https://mirrors.ukfast.co.uk/sites/qt.io",
        "https://mirrors.ocf.berkeley.edu/qt",
        "https://ftp.fau.de/qtproject",
        "https://mirrors.dotsrc.org/qtproject",
        "https://mirrors.aliyun.com/qt"
    )
    foreach ($base in $mirrors) {
        $args = @("-c", $script:AqtConfig, "install-qt") + $CoreArgs + @(
            "-O", $QtRoot, "-b", $base, "--timeout", "90"
        )
        Log "Try $What base=$base"
        if (Invoke-Aqt $args) { return $true }
        if ($script:LastAqtOutput -match "packages .+ were not found") {
            Log "Skipping remaining mirrors: arch/packages not available for this Qt version"
            return $false
        }
    }
    return $false
}

function Ensure-Qt {
    Write-AqtConfig
    New-Item -ItemType Directory -Force -Path $QtRoot | Out-Null

    $hostQmake = Find-FirstFile @(
        (Join-Path $QtRoot "$QtVer\mingw*\bin\qmake.exe"),
        (Join-Path $QtRoot "$QtVer\win64_mingw*\bin\qmake.exe"),
        (Join-Path $QtRoot "$QtVer\msvc*\bin\qmake.exe"),
        (Join-Path $QtRoot "$QtVer\win64_msvc*\bin\qmake.exe")
    )
    if (-not $hostQmake) {
        # Qt 6.5.x: win64_mingw / win64_msvc2019_64
        $hostArchs = @("win64_mingw", "win64_msvc2019_64")
        $ok = $false
        foreach ($arch in $hostArchs) {
            Log "Fetching Qt $QtVer host ($arch)…"
            if (Invoke-AqtInstallQt -What "host $arch" -CoreArgs @(
                    "windows", "desktop", $QtVer, $arch, "-m", "qtmultimedia"
                )) {
                $ok = $true
                break
            }
        }
        if (-not $ok) {
            Die "Failed to download Qt $QtVer host tools (mirrors/checksum). Re-run later or set NYX_QT_ROOT to an existing Qt."
        }
        $hostQmake = Find-FirstFile @(
            (Join-Path $QtRoot "$QtVer\mingw*\bin\qmake.exe"),
            (Join-Path $QtRoot "$QtVer\win64_mingw*\bin\qmake.exe"),
            (Join-Path $QtRoot "$QtVer\msvc*\bin\qmake.exe"),
            (Join-Path $QtRoot "$QtVer\win64_msvc*\bin\qmake.exe")
        )
    }
    if (-not $hostQmake) { Die "Qt host qmake not found under $QtRoot\$QtVer" }
    $env:QT_HOST_PATH = (Resolve-Path (Join-Path (Split-Path $hostQmake) "..")).Path

    $andrCmake = Find-FirstFile @(
        (Join-Path $QtRoot "$QtVer\android_arm64_v8a\bin\qt-cmake.bat")
    )
    if (-not $andrCmake) {
        Log "Fetching Qt $QtVer Android (android_arm64_v8a)…"
        if (-not (Invoke-AqtInstallQt -What "android_arm64_v8a" -CoreArgs @(
                    "windows", "android", $QtVer, "android_arm64_v8a",
                    "-m", "qtmultimedia", "--autodesktop"
                ))) {
            Die "Failed to download Qt $QtVer Android. Re-run later."
        }
        $andrCmake = Find-FirstFile @(
            (Join-Path $QtRoot "$QtVer\android_arm64_v8a\bin\qt-cmake.bat")
        )
    }
    if (-not $andrCmake) { Die "qt-cmake.bat not found under $QtRoot\$QtVer\android_arm64_v8a" }
    $script:QtAndroid = (Resolve-Path (Join-Path (Split-Path $andrCmake) "..")).Path

    Log "Qt Android: $script:QtAndroid"
    Log "Qt host:    $($env:QT_HOST_PATH)"
}

function Ensure-Perl {
    Refresh-Path
    $perl = Find-FirstFile @(
        "C:\Strawberry\perl\bin\perl.exe",
        "${env:ProgramFiles}\Strawberry\perl\bin\perl.exe"
    )
    if (-not $perl) {
        $cmd = Get-Command perl -ErrorAction SilentlyContinue
        if ($cmd) { $perl = $cmd.Source }
    }
    if (-not $perl) { Die "Perl not found (needed for libaom)." }
    $env:Path = "$(Split-Path $perl);$env:Path"
    Log "Perl: $perl"
}

function Ensure-CMake-Ninja {
    Refresh-Path
    $cmake = Find-FirstFile @(
        "${env:ProgramFiles}\CMake\bin\cmake.exe",
        "${env:ProgramFiles}\WinGet\Links\cmake.exe"
    )
    if (-not $cmake) {
        $cmd = Get-Command cmake -ErrorAction SilentlyContinue
        if ($cmd) { $cmake = $cmd.Source }
    }
    if (-not $cmake) { Die "cmake not found." }
    $env:Path = "$(Split-Path $cmake);$env:Path"

    $ninja = Get-Command ninja -ErrorAction SilentlyContinue
    if (-not $ninja) {
        $ninjaPath = Find-FirstFile @(
            "${env:LocalAppData}\Microsoft\WinGet\Links\ninja.exe",
            "${env:LocalAppData}\Microsoft\WinGet\Packages\Ninja-build.Ninja*\ninja.exe"
        )
        if ($ninjaPath) { $env:Path = "$(Split-Path $ninjaPath);$env:Path" }
    }
    if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) { Die "ninja not found." }
    Log "cmake: $((Get-Command cmake).Source)"
    Log "ninja: $((Get-Command ninja).Source)"
}

function Ensure-DebugKeystore {
    $dir = Split-Path $Keystore -Parent
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    if (Test-Path $Keystore) { return }

    $keytool = Join-Path $env:JAVA_HOME "bin\keytool.exe"
    if (-not (Test-Path $keytool)) { Die "keytool not found in JAVA_HOME" }
    Log "Creating debug keystore at $Keystore…"
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        & $keytool -genkeypair -v `
            -keystore $Keystore `
            -storepass $StorePass `
            -keypass $KeyPass `
            -alias $KeyAlias `
            -keyalg RSA -keysize 2048 -validity 10000 `
            -dname "CN=Android Debug,O=Android,C=US"
        if ($LASTEXITCODE -ne 0) { Die "keytool failed" }
    } finally {
        $ErrorActionPreference = $prev
    }
}

function Configure-And-Build {
    Ensure-CMake-Ninja
    Ensure-Perl
    $qtCmake = Join-Path $script:QtAndroid "bin\qt-cmake.bat"
    if (-not (Test-Path $qtCmake)) { Die "qt-cmake.bat not found: $qtCmake" }
    Log "Configuring $BuildDir…"
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        Remove-Item (Join-Path $BuildDir "CMakeCache.txt") -ErrorAction SilentlyContinue
        Remove-Item (Join-Path $BuildDir "CMakeFiles") -Recurse -Force -ErrorAction SilentlyContinue
        $env:NYX_QT_ANDROID_ROOT = $script:QtAndroid
        $env:NYX_QT_HOST_ROOT = $env:QT_HOST_PATH
        & $qtCmake -B $BuildDir -G Ninja `
            "-DANDROID_SDK_ROOT=$($env:ANDROID_SDK_ROOT)" `
            "-DANDROID_NDK_ROOT=$($env:ANDROID_NDK_ROOT)" `
            "-DCMAKE_BUILD_TYPE=Release" `
            "-DQT_HOST_PATH=$($env:QT_HOST_PATH)" `
            "-DNYX_QT_ANDROID_ROOT=$($script:QtAndroid)" `
            "-DNYX_QT_HOST_ROOT=$($env:QT_HOST_PATH)"
        if ($LASTEXITCODE -ne 0) { Die "qt-cmake configure failed" }
        Log "Building…"
        & cmake --build $BuildDir --parallel
        if ($LASTEXITCODE -ne 0) { Die "cmake --build failed" }
    } finally {
        $ErrorActionPreference = $prev
    }
}

function Sign-Apk {
    Ensure-DebugKeystore
    $deploySettings = Join-Path $BuildDir "android-nyx-app-deployment-settings.json"
    if (-not (Test-Path $deploySettings)) {
        $alt = Get-ChildItem $BuildDir -Filter "*deployment-settings.json" -Recurse -ErrorAction SilentlyContinue |
            Select-Object -First 1 -ExpandProperty FullName
        if ($alt) { $deploySettings = $alt }
    }
    if (-not (Test-Path $deploySettings)) { Die "missing android deployment settings under $BuildDir" }

    $androiddeployqt = Join-Path $env:QT_HOST_PATH "bin\androiddeployqt.exe"
    if (-not (Test-Path $androiddeployqt)) { Die "androiddeployqt.exe not found at $androiddeployqt" }
    $androidBuild = Join-Path $BuildDir "android-build"
    Log "Packaging and signing APK…"
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        & $androiddeployqt `
            --input $deploySettings `
            --output $androidBuild `
            --apk $OutApk `
            --release `
            --sign $Keystore $KeyAlias `
            --storepass $StorePass `
            --keypass $KeyPass
        if ($LASTEXITCODE -ne 0) { Die "androiddeployqt failed" }
    } finally {
        $ErrorActionPreference = $prev
    }
    if (-not (Test-Path $OutApk)) { Die "APK was not produced at $OutApk" }
    Log "Done: $OutApk"
}

function Install-ApkToDevice {
    $adb = Find-FirstFile @(
        "${env:LocalAppData}\Android\Sdk\platform-tools\adb.exe",
        "${env:LocalAppData}\Microsoft\WinGet\Packages\Google.PlatformTools*\platform-tools\adb.exe",
        "${env:LocalAppData}\Microsoft\WinGet\Links\adb.exe"
    )
    if (-not $adb) {
        $cmd = Get-Command adb -ErrorAction SilentlyContinue
        if ($cmd) { $adb = $cmd.Source }
    }
    if (-not $adb) { Die "adb not found; cannot install APK" }
    Log "Installing $OutApk …"
    $prev = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        & $adb install -r $OutApk | Out-Host
        if ($LASTEXITCODE -ne 0) { Die "adb install failed" }
    } finally {
        $ErrorActionPreference = $prev
    }
    Log "Installed on device."
}

Log "Root: $Root"
Ensure-Tools
Pick-Java17
$python = Ensure-Python
Ensure-Sdk
Ensure-Aqt $python
Ensure-Qt
Ensure-DebugKeystore

if ($DepsOnly) {
    Log "Dependencies ready."
    exit 0
}

Configure-And-Build
Sign-Apk
Log "APK: $OutApk"
if ($InstallApk) { Install-ApkToDevice }
Log "Finished."
