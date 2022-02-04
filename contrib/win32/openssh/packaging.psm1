# Copyright (c) Microsoft Corporation.
# Licensed under the MIT License.

# from powershell/powershell repo

enum PackageManifestResultStatus {
    Mismatch
    Match
    MissingFromManifest
    MissingFromPackage
}

class PackageManifestResult {
    [string] $File
    [string] $ExpectedHash
    [string] $ActualHash
    [PackageManifestResultStatus] $Status
}

function Test-PackageManifest {
    param (
        [Parameter(Mandatory)]
        [string]
        $PackagePath
    )

    Begin {
        $spdxManifestPath = Join-Path $PackagePath -ChildPath "/_manifest/spdx_2.2/manifest.spdx.json"
        $man = Get-Content $spdxManifestPath -ErrorAction Stop | convertfrom-json
        $inManifest = @()
    }

    Process {
        Write-Verbose "Processing $($man.files.count) files..." -verbose
        $man.files | ForEach-Object {
            $filePath = Join-Path $PackagePath -childPath $_.fileName
            $checksumObj = $_.checksums | Where-Object {$_.algorithm -eq 'sha256'}
            $sha256 = $checksumObj.checksumValue
            $actualHash = $null
            $actualHash = (Get-FileHash -Path $filePath -Algorithm sha256 -ErrorAction SilentlyContinue).Hash
            $inManifest += $filePath
            if($actualHash -ne $sha256) {
                $status = [PackageManifestResultStatus]::Mismatch
                if (!$actualHash) {
                    $status = [PackageManifestResultStatus]::MissingFromPackage
                }
                [PackageManifestResult] $result = @{
                    File         = $filePath
                    ExpectedHash = $sha256
                    ActualHash   = $actualHash
                    Status       = $status
                }
                Write-Output $result
            }
            else {
                [PackageManifestResult] $result = @{
                    File         = $filePath
                    ExpectedHash = $sha256
                    ActualHash   = $actualHash
                    Status       = [PackageManifestResultStatus]::Match
                }
                Write-Output $result
            }
        }


        Get-ChildItem $PackagePath -recurse | Select-Object -ExpandProperty FullName | foreach-object {
            if(!$inManifest -contains $_) {
                $actualHash = (get-filehash -Path $_ -algorithm sha256 -erroraction silentlycontinue).Hash
                [PackageManifestResult] $result = @{
                    File         = $_
                    ExpectedHash = $null
                    ActualHash   = $actualHash
                    Status       = [PackageManifestResultStatus]::MissingFromManifest
                }
                Write-Output $result
            }
        }
    }
}
