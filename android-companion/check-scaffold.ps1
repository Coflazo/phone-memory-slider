$ErrorActionPreference = 'Stop'

[xml](Get-Content "$PSScriptRoot/app/src/main/AndroidManifest.xml" -Raw) | Out-Null
[xml](Get-Content "$PSScriptRoot/app/src/main/res/values/styles.xml" -Raw) | Out-Null

$sourceFiles = Get-ChildItem "$PSScriptRoot/app/src/main" -Recurse -File
$requiredTokens = @(
    'MediaCatalogSource',
    'PairingSession',
    'TrashCoordinator',
    'MediaStore.createTrashRequest',
    'READ_MEDIA_VISUAL_USER_SELECTED',
    'foregroundServiceType'
)

foreach ($token in $requiredTokens) {
    if (-not ($sourceFiles | Select-String -Pattern $token -SimpleMatch -Quiet)) {
        throw "Android scaffold is missing required token: $token"
    }
}

Write-Output 'Android scaffold static checks passed'
