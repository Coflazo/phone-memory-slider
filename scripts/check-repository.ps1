$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$tokens = Get-Content (Join-Path $root 'design/tokens.json') -Raw | ConvertFrom-Json
$design = Get-Content (Join-Path $root 'DESIGN.md') -Raw
$qml = Get-Content (Join-Path $root 'desktop/qml/Theme.qml') -Raw

foreach ($property in $tokens.color.PSObject.Properties) {
    foreach ($mode in @('dark', 'light')) {
        $hex = $property.Value.$mode
        if ($design -notmatch [regex]::Escape($hex)) {
            throw "DESIGN.md is missing $($property.Name).$mode ($hex)"
        }
        if ($qml -notmatch [regex]::Escape($hex)) {
            throw "Theme.qml is missing $($property.Name).$mode ($hex)"
        }
    }
}

foreach ($property in $tokens.motionMs.PSObject.Properties) {
    if ($qml -notmatch "\b$($property.Value)\b") {
        throw "Theme.qml is missing motion token $($property.Name)"
    }
}

$runtimeFiles = Get-ChildItem @((Join-Path $root 'core'), (Join-Path $root 'desktop')) -Recurse -File |
    Where-Object { $_.Extension -in @('.cpp', '.hpp', '.qml') -and $_.FullName -notmatch '[\\/]tests[\\/]' }

$forbiddenRuntime = $runtimeFiles | Select-String -Pattern @(
    'Qt6::Network',
    '#include\s*[<"]QNetwork',
    '\bQNetwork[A-Za-z]+',
    '\bQTcpSocket\b',
    '\bQUdpSocket\b',
    '\bQWebSocket\b',
    '\bWinHttp[A-Za-z]+',
    '\bcurl_easy_',
    'https?://'
)
if ($forbiddenRuntime) {
    $first = $forbiddenRuntime | Select-Object -First 1
    throw "Runtime egress primitive found: $($first.Path):$($first.LineNumber)"
}

$androidManifest = Get-Content (Join-Path $root 'android-companion/app/src/main/AndroidManifest.xml') -Raw
foreach ($permission in @(
    'android.permission.INTERNET',
    'android.permission.ACCESS_NETWORK_STATE',
    'android.permission.CHANGE_WIFI_STATE',
    'android.permission.CHANGE_WIFI_MULTICAST_STATE'
)) {
    if ($androidManifest.Contains($permission)) {
        throw "Android runtime egress permission found: $permission"
    }
}

$androidRuntime = Get-ChildItem (Join-Path $root 'android-companion/app/src/main') -Recurse -File |
    Where-Object { $_.Extension -in @('.kt', '.java') }
$forbiddenAndroid = $androidRuntime | Select-String -Pattern @(
    '\bjava\.net\.(Socket|ServerSocket|URL|HttpURLConnection)\b',
    '\bokhttp3\.',
    '\bretrofit2\.',
    'https?://'
)
if ($forbiddenAndroid) {
    $first = $forbiddenAndroid | Select-Object -First 1
    throw "Android IP/network primitive found: $($first.Path):$($first.LineNumber)"
}

$requiredReleaseFiles = @(
    'LICENSE',
    'THIRD_PARTY_NOTICES.md',
    'third_party/geist/OFL.txt',
    'docs/USER_GUIDE.md',
    'docs/PRIVACY.md',
    'docs/SECURITY.md',
    'docs/RELEASE_CHECKLIST.md'
)
foreach ($relativePath in $requiredReleaseFiles) {
    if (-not (Test-Path -LiteralPath (Join-Path $root $relativePath))) {
        throw "Release asset is missing: $relativePath"
    }
}

$bluetoothTokens = @(
    'QBluetoothServer',
    'createRfcommSocketToServiceRecord',
    'pms://pair',
    'MediaStore.MediaColumns.IS_FAVORITE'
)
$allRuntimeText = @(
    Get-Content (Join-Path $root 'desktop/src/phone_client.cpp') -Raw
    Get-Content (Join-Path $root 'android-companion/app/src/main/kotlin/app/pms/companion/BluetoothGallerySession.kt') -Raw
    Get-Content (Join-Path $root 'android-companion/app/src/main/kotlin/app/pms/companion/PairingPayload.kt') -Raw
    Get-Content (Join-Path $root 'android-companion/app/src/main/kotlin/app/pms/companion/MediaStoreCatalogSource.kt') -Raw
) -join "`n"
foreach ($token in $bluetoothTokens) {
    if (-not $allRuntimeText.Contains($token)) {
        throw "Bluetooth offline boundary is missing required token: $token"
    }
}

Write-Output 'Repository consistency and zero-runtime-egress checks passed'
