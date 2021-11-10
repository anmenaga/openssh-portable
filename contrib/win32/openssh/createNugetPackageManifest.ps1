#This script updates the nuget package manifest
[cmdletbinding(DefaultParameterSetName='Build')]
param(
    [Parameter(Mandatory,ParameterSetName='Build')]
    [String]$Architecture="X64",
    [Parameter(Mandatory)]
    [string]$ManifestDir,
    [Parameter(Mandatory)]
    [string]$BinaryPath,
    [Parameter(Mandatory)]
    [string]$PackageVersion,
    [Parameter(Mandatory)]
    [string]$OpenSSHVersion
)

# Add required metadata elements
$id = "Win32-OpenSSH-$Architecture"
$version = $PackageVersion
$description = "This package contains the binary files for running OpenSSH for Windows."
$authors = "Microsoft"
$projectUrl = "https://github.com/PowerShell/Win32-OpenSSH"

# Write XML file
$xmlSettings = New-Object System.Xml.XmlWriterSettings
$xmlSettings.Indent = $true
$xmlSettings.IndentChars = "    "


$ManifestFilename = "Win32-OpenSSH-$Architecture.nuspec"
Write-Verbose $ManifestFilename -Verbose
$ManifestPath = Join-Path $ManifestDir $ManifestFilename
Write-Verbose $ManifestPath -Verbose
$xmlWriter = [System.XML.XmlWriter]::Create($ManifestPath, $xmlSettings)

$xmlWriter.WriteStartDocument()

$xmlWriter.WriteStartElement("package", "http://schemas.microsoft.com/packaging/2010/07/nuspec.xsd")

    $xmlWriter.WriteStartElement("metadata")
        $xmlWriter.WriteElementString("id", $id)
        $xmlWriter.WriteElementString("version", $version)
        $xmlWriter.WriteElementString("authors", $authors)
        $xmlWriter.WriteElementString("description", $description)
        $xmlWriter.WriteElementString("projectUrl", $projectUrl)
        $xmlWriter.WriteElementString("releaseNotes", "OpenSSH For Windows $OpenSSHVersion")
    $xmlWriter.WriteEndElement()

    $xmlWriter.WriteStartElement("files")
    
        $filepath = Join-Path $BinaryPath *
        $xmlWriter.WriteStartElement("file")
            $xmlWriter.WriteAttributeString("src", $filepath)
            $xmlWriter.WriteAttributeString("target", "")
        $xmlWriter.WriteEndElement()

    $xmlWriter.WriteEndElement()


$xmlWriter.WriteEndElement()

$xmlWriter.WriteEndDocument()
$xmlWriter.Flush()
$xmlWriter.Close()

exit 0