$ErrorActionPreference = 'Stop'

[xml](Get-Content "$PSScriptRoot/app/src/main/AndroidManifest.xml" -Raw) | Out-Null
[xml](Get-Content "$PSScriptRoot/app/src/main/res/values/styles.xml" -Raw) | Out-Null

$sourceFiles = Get-ChildItem "$PSScriptRoot/app/src/main" -Recurse -File
$requiredTokens = @(
    'MediaCatalogSource',
    'BluetoothGallerySession',
    'createRfcommSocketToServiceRecord',
    'PairingPayload',
    'TrashCoordinator',
    'MediaStore.createTrashRequest',
    'READ_MEDIA_VISUAL_USER_SELECTED',
    'BLUETOOTH_CONNECT'
)

foreach ($token in $requiredTokens) {
    if (-not ($sourceFiles | Select-String -Pattern $token -SimpleMatch -Quiet)) {
        throw "Android scaffold is missing required token: $token"
    }
}

$manifest = Get-Content "$PSScriptRoot/app/src/main/AndroidManifest.xml" -Raw
foreach ($forbidden in @('android.permission.INTERNET', 'android.permission.ACCESS_NETWORK_STATE', 'android.permission.CHANGE_WIFI_STATE')) {
    if ($manifest.Contains($forbidden)) {
        throw "Android runtime egress permission found: $forbidden"
    }
}

Write-Output 'Android scaffold static checks passed'
