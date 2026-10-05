param(
    [Parameter(Mandatory = $true)]
    [string]$Package
)

$ErrorActionPreference = 'Stop'
$archivePath = (Resolve-Path -LiteralPath $Package).Path
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::OpenRead($archivePath)
try {
    $entries = @($archive.Entries | ForEach-Object { $_.FullName.Replace('\', '/').ToLowerInvariant() })
    foreach ($pattern in @(
        '/plugins/networkinformation/',
        '/plugins/tls/',
        '/plugins/qmltooling/',
        'qmldbg_tcp'
    )) {
        if ($entries | Where-Object { $_.Contains($pattern) }) {
            throw "Network-capability plugin found in package: $pattern"
        }
    }

    foreach ($required in @(
        '/bin/phone_memory_slider',
        '/license',
        '/third_party_notices.md',
        '/privacy.md',
        '/user_guide.md'
    )) {
        if (-not ($entries | Where-Object { $_.Contains($required) })) {
            throw "Required packaged asset is missing: $required"
        }
    }
} finally {
    $archive.Dispose()
}

Write-Output 'Package contents and offline plugin boundary passed'
