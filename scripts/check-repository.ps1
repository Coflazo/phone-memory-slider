$ErrorActionPreference = 'Stop'

$root = Split-Path -Parent $PSScriptRoot
$tokensPath = Join-Path $root 'design/tokens.json'
$tokens = Get-Content $tokensPath -Raw | ConvertFrom-Json
$design = Get-Content (Join-Path $root 'DESIGN.md') -Raw
$qml = Get-Content (Join-Path $root 'desktop/qml/Theme.qml') -Raw
$compose = Get-Content (Join-Path $root 'android-companion/app/src/main/kotlin/app/pms/companion/PmsTokens.kt') -Raw

foreach ($property in $tokens.color.PSObject.Properties) {
    foreach ($mode in @('dark', 'light')) {
        $hex = $property.Value.$mode
        if ($design -notmatch [regex]::Escape($hex)) {
            throw "DESIGN.md is missing $($property.Name).$mode ($hex)"
        }
        if ($qml -notmatch [regex]::Escape($hex)) {
            throw "Theme.qml is missing $($property.Name).$mode ($hex)"
        }
        $composeHex = '0xFF' + $hex.TrimStart('#')
        if ($compose -notmatch [regex]::Escape($composeHex)) {
            throw "PmsTokens.kt is missing $($property.Name).$mode ($composeHex)"
        }
    }
}

foreach ($property in $tokens.motionMs.PSObject.Properties) {
    if ($qml -notmatch "\b$($property.Value)\b") {
        throw "Theme.qml is missing motion token $($property.Name)"
    }
}

$runtimeRoots = @(
    (Join-Path $root 'core'),
    (Join-Path $root 'desktop'),
    (Join-Path $root 'android-companion/app/src/main/kotlin')
)
$runtimeFiles = Get-ChildItem $runtimeRoots -Recurse -File |
    Where-Object { $_.Extension -in @('.cpp', '.hpp', '.qml', '.kt') -and $_.FullName -notmatch '[\\/]tests[\\/]' }
$endpoint = $runtimeFiles | Select-String -Pattern 'https?://(?!local\b)(?!127\.0\.0\.1\b)(?!10\.)(?!192\.168\.)(?!172\.(1[6-9]|2\d|3[01])\.)[A-Za-z0-9\[]+' -CaseSensitive
if ($endpoint) {
    throw "Runtime source contains an external endpoint: $($endpoint.Path):$($endpoint.LineNumber)"
}

$protocol = Get-Content (Join-Path $root 'protocol/pms.proto') -Raw
if ($protocol -notmatch 'syntax = "proto3";' -or $protocol -notmatch 'package pms\.protocol\.v1;') {
    throw 'Protocol v1 declaration is missing'
}

$requiredReleaseFiles = @(
    'LICENSE',
    'THIRD_PARTY_NOTICES.md',
    'third_party/geist/OFL.txt',
    'docs/USER_GUIDE.md',
    'docs/PRIVACY.md',
    'docs/RELEASE_CHECKLIST.md',
    'android-companion/app/src/main/res/xml/data_extraction_rules.xml',
    'android-companion/app/src/main/res/xml/network_security_config.xml'
)
foreach ($relativePath in $requiredReleaseFiles) {
    if (-not (Test-Path -LiteralPath (Join-Path $root $relativePath))) {
        throw "Release asset is missing: $relativePath"
    }
}

$manifest = Get-Content (Join-Path $root 'android-companion/app/src/main/AndroidManifest.xml') -Raw
foreach ($requiredSetting in @('android:allowBackup="false"', 'android:fullBackupContent="false"', 'android:usesCleartextTraffic="false"')) {
    if ($manifest -notmatch [regex]::Escape($requiredSetting)) {
        throw "Android release safety setting is missing: $requiredSetting"
    }
}

& (Join-Path $root 'android-companion/check-scaffold.ps1')

Write-Output 'Repository consistency checks passed'
