[CmdletBinding(PositionalBinding = $false)]
param(
    [switch] $LibraryOnly,
    [switch] $CheckOnly,
    [switch] $Install,
    [switch] $NonInteractive,
    [switch] $Quiet,
    [switch] $PauseWhenDone,
    [string] $SearchDirectory = ""
)

$ErrorActionPreference = "Stop"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$UpdaterVersion = [version]"1.0.0"
$MaximumMetadataBytes = 1MB
$MaximumPackageBytes = 100MB
$MaximumExtractedBytes = 150MB
$MaximumArchiveEntries = 40
$MaximumSearchDepth = 2
$MaximumSearchDirectories = 250
$RetainedBackups = 1
$RetainedLogs = 2

$Products = @(
    [pscustomobject]@{
        Id = "OnjResearch.SYXG100Hybrid"
        DisplayName = "S-YXG100 Hybrid"
        Slug = "syxg100-hybrid"
        Repository = "OnjLouis/syxg100-hybrid"
        AnchorFile = "syxg100-hybrid.dll"
        VersionFile = "syxg100-hybrid.version.json"
        PublicKeyXml = '<RSAKeyValue><Modulus>spcmbu8AcaBqxN07TTcUPbpIGI4/oi6kLhe9ELY+/Aje38nLf3cA1kN6fceCtyeLqK8MhHrCOYgfNBIsE+roBgl5XUEfhNCv91wYIOnUPGD6Rgn2VvW3Qp+fsWb7FLD6J2YKhdjw5sMBVk87xtLtL+zMU3iziyQhimxNrX92kvdfr3bSSHJi+xo1sCFusZ7DMNgCpIkW6+6EBG79tyeVVvuZpk3oGiuV8Og5EFbL15stztWimWk4ZRy3OtTkxH3uyaEbfxfMr+FsmLtTqgoSuxvWPp7u5chlGMJgrbG3w7nA4w8aZddU976Vitl0j34+2HEUGZoXAm7oykh6AtnESWsyZU2yDRqHiz8EXd6mhvuxxu2L4ZudeoCy7+FpCnQ1cJzKTpOfPnrOfEGEnNfc5jGQqlvCviL2Xxfq330ay/ubMAOmjzkQC5Jsahb4RWV0a9C4lcf+RlbztAwdf9L02mOeEHQmF/51yciQ6DYjqDDVcCDI3q3dJLuSDkakWlrh</Modulus><Exponent>AQAB</Exponent></RSAKeyValue>'
        AllowedTargetFiles = @(
            "Sxgpvknl.vxd",
            "sxgsgknl.vxd",
            "syxg100-hybrid.dll",
            "syxg100-sg-worker.exe",
            "syxg100-vl-worker.exe",
            "syxg50-engine.bin",
            "syxg100-hybrid.version.json"
        )
    },
    [pscustomobject]@{
        Id = "OnjResearch.SYXG2026Hybrid"
        DisplayName = "S-YXG2026 Hybrid"
        Slug = "syxg2026-hybrid"
        Repository = "OnjLouis/syxg2026-hybrid"
        AnchorFile = "syxg2026-hybrid.dll"
        VersionFile = "syxg2026-hybrid.version.json"
        PublicKeyXml = '<RSAKeyValue><Modulus>vidOAREgvkf3Glua/rXJJukqIdzcy8fKg2YqMEVUVBHTt78YpmuuByymXc3sCAt367Hp5f3hhV5Jq6029tRmPMHd9juVHSOHeBGHipp7QMYoNh7v1Q9c6cSedyxOqZuK00QhgSzeLT76nVaOETVLbknf4NI8Mor7C2l4y1NYa58Z1mLgORqwjJ0InpecM9/O+KkjET+k7egcvTYCZ4MCmfGQ9UfS/edfa32XAaa8yeBkb1jlixDP/DcRVnMvwnPAG8f3LbtuYNjWf4T3p7qoLU6xnxq0M3Pw6KUj9AB6XUnJqwO26HHLHgbglzA4uruWfMwKCLenf7TQAPWh0NbQxf3Y13XUcvbO8V4eN9BAc+XeDY73HBE6z0g25mKkAtcWu+SGUquC07QAIvxFXzXBkkkHhbwbysvmpeY4QXua4llSRIWN8AN+x20/cMWPUIGEWXOBlraneIGq4Y1thgUbb4fsnWPh+IEBhazPw7p5wOW+Ff90jJlXd055B+IN+l05</Modulus><Exponent>AQAB</Exponent></RSAKeyValue>'
        AllowedTargetFiles = @(
            "sxgbnw6l.tbl",
            "sxgdat6l.tbl",
            "Sxgpvknl.vxd",
            "sxgsgknl.vxd",
            "syxg2006le-engine.bin",
            "syxg2026-hybrid.dll",
            "syxg2026-sg-worker.exe",
            "syxg2026-vl-worker.exe",
            "syxg50-engine.bin",
            "syxg2026-hybrid.version.json"
        )
    }
)

$AllowedProductFiles = @("README.html", "TESTER-NOTES.md")
$AllowedUpdaterFiles = @("Update Yamaha Hybrids.cmd", "Update-YamahaHybrids.ps1")
$ScriptDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path
$StateRoot = Join-Path ([Environment]::GetFolderPath([Environment+SpecialFolder]::LocalApplicationData)) "Onj Research\Yamaha Hybrid Updater"
if ([string]::IsNullOrWhiteSpace($SearchDirectory)) {
    $SearchDirectory = $ScriptDirectory
}
$ResolvedSearchDirectory = [System.IO.Path]::GetFullPath($SearchDirectory).TrimEnd('\')

function Write-UpdaterStatus {
    param(
        [string] $Message,
        [string] $LogPath = "",
        [switch] $Always
    )

    if (-not $Quiet -or $Always) {
        Write-Host $Message
    }
    if (-not [string]::IsNullOrWhiteSpace($LogPath)) {
        $Message | Out-File -LiteralPath $LogPath -Append -Encoding utf8
    }
}

function Convert-ToVersion {
    param([string] $Value)

    $clean = $Value.Trim().TrimStart('v', 'V')
    if ($clean -notmatch '^\d+\.\d+\.\d+$') {
        throw "Invalid release version: $Value"
    }
    return [version]$clean
}

function Get-ProductStateDirectory {
    param([object] $Product)

    return Join-Path $StateRoot $Product.Slug
}

function Get-HttpsBytes {
    param(
        [string] $Url,
        [long] $MaximumBytes
    )

    $uri = [uri]$Url
    if ($uri.Scheme -ne "https") {
        throw "The update service supplied a non-HTTPS address."
    }

    Add-Type -AssemblyName System.Net.Http
    $handler = [System.Net.Http.HttpClientHandler]::new()
    $client = [System.Net.Http.HttpClient]::new($handler)
    $client.Timeout = [TimeSpan]::FromSeconds(120)
    $client.DefaultRequestHeaders.UserAgent.ParseAdd("OnjResearch-YamahaHybridUpdater/$($UpdaterVersion.ToString(3))")
    $client.DefaultRequestHeaders.CacheControl = [System.Net.Http.Headers.CacheControlHeaderValue]::new()
    $client.DefaultRequestHeaders.CacheControl.NoCache = $true
    if ($uri.Host -ieq "api.github.com" -and -not [string]::IsNullOrWhiteSpace($env:GH_TOKEN)) {
        $client.DefaultRequestHeaders.Authorization = [System.Net.Http.Headers.AuthenticationHeaderValue]::new("Bearer", $env:GH_TOKEN.Trim())
    }

    $response = $null
    $stream = $null
    $memory = [System.IO.MemoryStream]::new()
    try {
        $response = $client.GetAsync($uri, [System.Net.Http.HttpCompletionOption]::ResponseHeadersRead).GetAwaiter().GetResult()
        $null = $response.EnsureSuccessStatusCode()
        if ($response.RequestMessage.RequestUri.Scheme -ne "https") {
            throw "The update download redirected outside HTTPS."
        }
        $declaredLength = $response.Content.Headers.ContentLength
        if ($declaredLength -and $declaredLength -gt $MaximumBytes) {
            throw "The update download was unexpectedly large."
        }
        $stream = $response.Content.ReadAsStreamAsync().GetAwaiter().GetResult()
        $buffer = New-Object byte[] 65536
        while (($read = $stream.Read($buffer, 0, $buffer.Length)) -gt 0) {
            if ($memory.Length + $read -gt $MaximumBytes) {
                throw "The update download exceeded its size limit."
            }
            $memory.Write($buffer, 0, $read)
        }
        return $memory.ToArray()
    }
    finally {
        if ($stream) { $stream.Dispose() }
        $memory.Dispose()
        if ($response) { $response.Dispose() }
        $client.Dispose()
        $handler.Dispose()
    }
}

function Get-ReleaseAsset {
    param(
        [object] $Release,
        [string] $Name
    )

    $matches = @($Release.assets | Where-Object { [string]$_.name -ceq $Name })
    if ($matches.Count -ne 1) {
        throw "Release asset is missing or duplicated: $Name"
    }
    $url = [string]$matches[0].browser_download_url
    if (-not $url.StartsWith("https://", [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Release asset did not use HTTPS: $Name"
    }
    return $url
}

function Get-LatestProductRelease {
    param([object] $Product)

    $apiUrl = "https://api.github.com/repos/$($Product.Repository)/releases?per_page=20&cachebust=$([DateTime]::UtcNow.Ticks)"
    $releaseBytes = Get-HttpsBytes -Url $apiUrl -MaximumBytes $MaximumMetadataBytes
    $parsed = [System.Text.Encoding]::UTF8.GetString($releaseBytes) | ConvertFrom-Json
    $candidates = @()
    foreach ($release in @($parsed)) {
        if ([bool]$release.draft -or [bool]$release.prerelease) {
            continue
        }
        try {
            $version = Convert-ToVersion ([string]$release.tag_name)
            $candidates += [pscustomobject]@{ Version = $version; Release = $release }
        }
        catch {
            continue
        }
    }
    if ($candidates.Count -eq 0) {
        return $null
    }

    $selected = $candidates | Sort-Object Version -Descending | Select-Object -First 1
    $versionText = $selected.Version.ToString(3)
    $manifestName = "$($Product.Slug)-$versionText.update.json"
    $signatureName = "$($Product.Slug)-$versionText.update.sig"
    $notes = ([string]$selected.Release.body) -replace '[\x00-\x08\x0B\x0C\x0E-\x1F\x7F]', ''
    if ($notes.Length -gt 6000) {
        $notes = $notes.Substring(0, 6000) + "`r`n[Release notes truncated]"
    }
    return [pscustomobject]@{
        Version = $selected.Version
        VersionText = $versionText
        PageUrl = [string]$selected.Release.html_url
        Notes = $notes.Trim()
        ManifestUrl = Get-ReleaseAsset -Release $selected.Release -Name $manifestName
        SignatureUrl = Get-ReleaseAsset -Release $selected.Release -Name $signatureName
    }
}

function Test-ManifestSignature {
    param(
        [byte[]] $ManifestBytes,
        [string] $SignatureText,
        [string] $PublicKeyXml
    )

    try {
        $signature = [Convert]::FromBase64String($SignatureText.Trim())
        $rsa = [System.Security.Cryptography.RSACryptoServiceProvider]::new()
        try {
            $rsa.FromXmlString($PublicKeyXml)
            return $rsa.VerifyData($ManifestBytes, "SHA256", $signature)
        }
        finally {
            $rsa.Dispose()
        }
    }
    catch {
        return $false
    }
}

function Get-AllowedFileNames {
    param(
        [object] $Product,
        [string] $Scope
    )

    switch ($Scope) {
        "target" { return @($Product.AllowedTargetFiles) }
        "product" { return @($AllowedProductFiles) }
        "updater" { return @($AllowedUpdaterFiles) }
        default { throw "The signed manifest contains an unknown destination: $Scope" }
    }
}

function Read-AndVerifyManifest {
    param(
        [byte[]] $ManifestBytes,
        [string] $SignatureText,
        [version] $ExpectedVersion,
        [object] $Product,
        [string] $PublicKeyXml = ""
    )

    if ([string]::IsNullOrWhiteSpace($PublicKeyXml)) {
        $PublicKeyXml = $Product.PublicKeyXml
    }
    if (-not (Test-ManifestSignature -ManifestBytes $ManifestBytes -SignatureText $SignatureText -PublicKeyXml $PublicKeyXml)) {
        throw "The update manifest signature is invalid. Nothing was installed."
    }

    $manifest = [System.Text.Encoding]::UTF8.GetString($ManifestBytes) | ConvertFrom-Json
    if ([int]$manifest.schemaVersion -ne 1 -or [string]$manifest.product -ne $Product.Id) {
        throw "The signed manifest belongs to a different synth or schema."
    }
    $manifestVersion = Convert-ToVersion ([string]$manifest.version)
    if ($manifestVersion -ne $ExpectedVersion) {
        throw "The signed manifest version does not match the GitHub release."
    }
    $manifestUpdaterVersion = Convert-ToVersion ([string]$manifest.updaterVersion)
    if ([string]$manifest.package.url -notmatch '^https://') {
        throw "The signed package address is not HTTPS."
    }
    if ([string]$manifest.package.sha256 -notmatch '^[a-fA-F0-9]{64}$') {
        throw "The signed package hash is invalid."
    }

    $files = @($manifest.files)
    if ($files.Count -eq 0 -or $files.Count -gt $MaximumArchiveEntries) {
        throw "The signed manifest contains an invalid file count."
    }
    $seenPaths = @{}
    foreach ($file in $files) {
        $scope = ([string]$file.scope).ToLowerInvariant()
        $name = [string]$file.name
        if ([string]::IsNullOrWhiteSpace($name) -or $name -ne [System.IO.Path]::GetFileName($name) -or $name.IndexOfAny([System.IO.Path]::GetInvalidFileNameChars()) -ge 0) {
            throw "The signed manifest contains an unsafe file name: $name"
        }
        $allowed = @(Get-AllowedFileNames -Product $Product -Scope $scope)
        if (-not ($allowed -ccontains $name)) {
            throw "The signed manifest attempted to manage an unexpected file: $scope/$name"
        }
        if ([string]$file.sha256 -notmatch '^[a-fA-F0-9]{64}$') {
            throw "The signed manifest contains an invalid file hash: $scope/$name"
        }
        $key = "$scope/$name".ToLowerInvariant()
        if ($seenPaths.ContainsKey($key)) {
            throw "The signed manifest contains a duplicate file: $scope/$name"
        }
        $seenPaths[$key] = $true
    }
    if (@($files | Where-Object { $_.scope -eq "target" -and $_.name -ceq $Product.AnchorFile }).Count -ne 1) {
        throw "The signed manifest does not contain the expected synth DLL."
    }
    if (@($files | Where-Object { $_.scope -eq "target" -and $_.name -ceq $Product.VersionFile }).Count -ne 1) {
        throw "The signed manifest does not contain version metadata."
    }

    return [pscustomobject]@{
        Raw = $manifest
        Version = $manifestVersion
        UpdaterVersion = $manifestUpdaterVersion
        Files = $files
        PackageUrl = [string]$manifest.package.url
        PackageHash = ([string]$manifest.package.sha256).ToLowerInvariant()
    }
}

function Find-SynthTargets {
    param([string] $RootDirectory = $ResolvedSearchDirectory)

    $root = [System.IO.Path]::GetFullPath($RootDirectory).TrimEnd('\')
    if (-not (Test-Path -LiteralPath $root -PathType Container)) {
        throw "Search folder not found: $root"
    }

    $queue = [System.Collections.Generic.Queue[object]]::new()
    $queue.Enqueue([pscustomobject]@{ Path = $root; Depth = 0 })
    $targets = New-Object System.Collections.Generic.List[object]
    $visited = 0
    while ($queue.Count -gt 0) {
        $item = $queue.Dequeue()
        $visited++
        if ($visited -gt $MaximumSearchDirectories) {
            throw "The synth search exceeded its safety limit. Run the updater from a narrower folder."
        }
        foreach ($product in $Products) {
            $anchorPath = Join-Path $item.Path $product.AnchorFile
            if (Test-Path -LiteralPath $anchorPath -PathType Leaf) {
                $leaf = Split-Path -Leaf $item.Path
                $productDirectory = if ($leaf -ieq "VST") { Split-Path -Parent $item.Path } else { $item.Path }
                $targets.Add([pscustomobject]@{
                    Product = $product
                    RuntimeDirectory = $item.Path
                    ProductDirectory = $productDirectory
                    AnchorPath = $anchorPath
                })
            }
        }
        if ($item.Depth -ge $MaximumSearchDepth) {
            continue
        }
        foreach ($directory in @(Get-ChildItem -LiteralPath $item.Path -Directory -Force -ErrorAction SilentlyContinue)) {
            $queue.Enqueue([pscustomobject]@{ Path = $directory.FullName; Depth = $item.Depth + 1 })
        }
    }
    return $targets.ToArray()
}

function Get-InstalledVersion {
    param([object] $Target)

    $path = Join-Path $Target.RuntimeDirectory $Target.Product.VersionFile
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        return [version]"0.0.0"
    }
    $metadata = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
    if ([string]$metadata.product -ne $Target.Product.Id) {
        throw "Version metadata in $($Target.RuntimeDirectory) belongs to a different synth."
    }
    return Convert-ToVersion ([string]$metadata.version)
}

function Get-DestinationDirectory {
    param(
        [object] $Target,
        [string] $Scope
    )

    switch ($Scope.ToLowerInvariant()) {
        "target" { return $Target.RuntimeDirectory }
        "product" { return $Target.ProductDirectory }
        "updater" { return $ScriptDirectory }
        default { throw "Unknown update destination: $Scope" }
    }
}

function Expand-AndValidatePackage {
    param(
        [string] $PackagePath,
        [string] $Destination,
        [object] $VerifiedManifest
    )

    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [System.IO.Compression.ZipFile]::OpenRead($PackagePath)
    try {
        $entries = @($archive.Entries)
        if ($entries.Count -ne $VerifiedManifest.Files.Count -or $entries.Count -gt $MaximumArchiveEntries) {
            throw "The update archive does not match its signed file list."
        }
        if (($entries | Measure-Object Length -Sum).Sum -gt $MaximumExtractedBytes) {
            throw "The extracted update would be unexpectedly large."
        }
        $expected = @{}
        foreach ($file in $VerifiedManifest.Files) {
            $expected[("$($file.scope)/$($file.name)").ToLowerInvariant()] = $file
        }
        $seenEntries = @{}
        foreach ($entry in $entries) {
            $entryPath = $entry.FullName.Replace('\', '/')
            $key = $entryPath.ToLowerInvariant()
            if ([string]::IsNullOrWhiteSpace($entry.Name) -or $entryPath.StartsWith('/') -or $entryPath.Split('/').Count -ne 2 -or -not $expected.ContainsKey($key) -or $seenEntries.ContainsKey($key)) {
                throw "The update archive contains an unexpected path: $entryPath"
            }
            $seenEntries[$key] = $true

            $outputPath = Join-Path $Destination $entryPath.Replace('/', '\')
            $outputDirectory = Split-Path -Parent $outputPath
            New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
            $input = $entry.Open()
            $output = [System.IO.File]::Create($outputPath)
            try {
                $input.CopyTo($output)
            }
            finally {
                $output.Dispose()
                $input.Dispose()
            }
        }
    }
    finally {
        $archive.Dispose()
    }
    foreach ($file in $VerifiedManifest.Files) {
        $path = Join-Path $Destination ("$($file.scope)\$($file.name)")
        $hash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($hash -ne ([string]$file.sha256).ToLowerInvariant()) {
            throw "A file in the update package failed verification: $($file.scope)/$($file.name)"
        }
        if ($file.name.EndsWith(".ps1", [System.StringComparison]::OrdinalIgnoreCase)) {
            $errors = $null
            $tokens = $null
            [void][System.Management.Automation.Language.Parser]::ParseFile($path, [ref]$tokens, [ref]$errors)
            if (@($errors).Count -gt 0) {
                throw "The update contains an invalid PowerShell script."
            }
        }
    }
}

function Remove-DirectoryWithRetry {
    param(
        [string] $Path,
        [int] $Attempts = 6
    )

    if (-not (Test-Path -LiteralPath $Path)) {
        return $true
    }
    for ($attempt = 1; $attempt -le $Attempts; $attempt++) {
        try {
            Remove-Item -LiteralPath $Path -Recurse -Force -ErrorAction Stop
            return $true
        }
        catch {
            if ($attempt -eq $Attempts) {
                return $false
            }
            Start-Sleep -Milliseconds (150 * $attempt)
        }
    }
    return $false
}

function New-RollbackArchive {
    param(
        [object] $Target,
        [object] $VerifiedManifest,
        [string] $StateDirectory,
        [version] $InstalledVersion
    )

    $work = Join-Path $StateDirectory ("work\backup-" + [guid]::NewGuid().ToString('N'))
    $backupContent = Join-Path $work "content"
    $backupDirectory = Join-Path $StateDirectory "backups"
    New-Item -ItemType Directory -Force -Path $backupContent, $backupDirectory | Out-Null
    $records = New-Object System.Collections.Generic.List[object]
    try {
        foreach ($file in $VerifiedManifest.Files) {
            if ($file.scope -eq "updater" -and $VerifiedManifest.UpdaterVersion -le $UpdaterVersion) {
                continue
            }
            $destinationDirectory = Get-DestinationDirectory -Target $Target -Scope $file.scope
            $destination = Join-Path $destinationDirectory $file.name
            $exists = Test-Path -LiteralPath $destination -PathType Leaf
            $records.Add([pscustomobject]@{ Scope = [string]$file.scope; Name = [string]$file.name; Existed = $exists })
            if ($exists) {
                $scopeDirectory = Join-Path $backupContent ([string]$file.scope)
                New-Item -ItemType Directory -Force -Path $scopeDirectory | Out-Null
                Copy-Item -LiteralPath $destination -Destination (Join-Path $scopeDirectory $file.name)
            }
        }
        $metadata = [ordered]@{
            product = $Target.Product.Id
            previousVersion = $InstalledVersion.ToString(3)
            createdUtc = [DateTime]::UtcNow.ToString("o")
            runtimeDirectory = $Target.RuntimeDirectory
            productDirectory = $Target.ProductDirectory
            updaterDirectory = $ScriptDirectory
            files = $records.ToArray()
        }
        [System.IO.File]::WriteAllText((Join-Path $backupContent "rollback.json"), ($metadata | ConvertTo-Json -Depth 5), [System.Text.UTF8Encoding]::new($false))
        $archivePath = Join-Path $backupDirectory ("$($Target.Product.Slug)-$($InstalledVersion.ToString(3))-" + (Get-Date -Format "yyyy-MM-dd_HHmmss_fff") + "-" + [guid]::NewGuid().ToString('N').Substring(0, 8) + ".zip")
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        [System.IO.Compression.ZipFile]::CreateFromDirectory($backupContent, $archivePath, [System.IO.Compression.CompressionLevel]::Optimal, $false)
        return [pscustomobject]@{ Path = $archivePath; Records = $records.ToArray() }
    }
    finally {
        $null = Remove-DirectoryWithRetry -Path $work
    }
}

function Restore-RollbackArchive {
    param(
        [object] $Target,
        [object] $Rollback
    )

    $stateDirectory = Get-ProductStateDirectory -Product $Target.Product
    $restoreRoot = Join-Path $stateDirectory ("work\restore-" + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force -Path $restoreRoot | Out-Null
    try {
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        [System.IO.Compression.ZipFile]::ExtractToDirectory($Rollback.Path, $restoreRoot)
        foreach ($record in $Rollback.Records) {
            $destinationDirectory = Get-DestinationDirectory -Target $Target -Scope $record.Scope
            $destination = Join-Path $destinationDirectory $record.Name
            if ([bool]$record.Existed) {
                $saved = Join-Path $restoreRoot ("$($record.Scope)\$($record.Name)")
                Copy-Item -LiteralPath $saved -Destination $destination -Force
            }
            elseif (Test-Path -LiteralPath $destination -PathType Leaf) {
                Remove-Item -LiteralPath $destination -Force
            }
        }
    }
    finally {
        $null = Remove-DirectoryWithRetry -Path $restoreRoot
    }
}

function Remove-OldState {
    param(
        [object] $Product,
        [string] $CurrentBackup = ""
    )

    $stateDirectory = Get-ProductStateDirectory -Product $Product
    $backupDirectory = Join-Path $stateDirectory "backups"
    if (Test-Path -LiteralPath $backupDirectory) {
        $backups = @(Get-ChildItem -LiteralPath $backupDirectory -File -Filter "*.zip" | Sort-Object LastWriteTime -Descending)
        if ([string]::IsNullOrWhiteSpace($CurrentBackup)) {
            $oldBackups = @($backups | Select-Object -Skip $RetainedBackups)
        }
        else {
            $oldBackups = @($backups | Where-Object { $_.FullName -ne $CurrentBackup })
        }
        foreach ($backup in $oldBackups) {
            Remove-Item -LiteralPath $backup.FullName -Force
        }
    }
    $logDirectory = Join-Path $stateDirectory "logs"
    if (Test-Path -LiteralPath $logDirectory) {
        foreach ($log in @(Get-ChildItem -LiteralPath $logDirectory -File -Filter "update-*.log" | Sort-Object LastWriteTime -Descending | Select-Object -Skip $RetainedLogs)) {
            Remove-Item -LiteralPath $log.FullName -Force
        }
    }
    $workDirectory = Join-Path $stateDirectory "work"
    if (Test-Path -LiteralPath $workDirectory) {
        foreach ($directory in @(Get-ChildItem -LiteralPath $workDirectory -Directory -Force -ErrorAction SilentlyContinue)) {
            $null = Remove-DirectoryWithRetry -Path $directory.FullName
        }
        if (@(Get-ChildItem -LiteralPath $workDirectory -Force -ErrorAction SilentlyContinue).Count -eq 0) {
            $null = Remove-DirectoryWithRetry -Path $workDirectory
        }
    }
}

function Install-VerifiedUpdate {
    param(
        [object] $Target,
        [byte[]] $ManifestBytes,
        [string] $SignatureText,
        [version] $ExpectedVersion,
        [byte[]] $PackageBytes,
        [string] $PublicKeyXml = ""
    )

    $product = $Target.Product
    $verified = Read-AndVerifyManifest -ManifestBytes $ManifestBytes -SignatureText $SignatureText -ExpectedVersion $ExpectedVersion -Product $product -PublicKeyXml $PublicKeyXml
    $packageHash = [System.Security.Cryptography.SHA256]::Create()
    try {
        $actualPackageHash = ([BitConverter]::ToString($packageHash.ComputeHash($PackageBytes))).Replace('-', '').ToLowerInvariant()
    }
    finally {
        $packageHash.Dispose()
    }
    if ($actualPackageHash -ne $verified.PackageHash) {
        throw "The update package failed its signed SHA-256 check. Nothing was installed."
    }
    if (-not (Test-Path -LiteralPath $Target.AnchorPath -PathType Leaf)) {
        throw "$($product.DisplayName) is no longer present in $($Target.RuntimeDirectory)."
    }

    $stateDirectory = Get-ProductStateDirectory -Product $product
    $workRoot = Join-Path $stateDirectory ("work\install-" + [guid]::NewGuid().ToString('N'))
    $packagePath = Join-Path $workRoot "package.zip"
    $extracted = Join-Path $workRoot "extracted"
    New-Item -ItemType Directory -Force -Path $extracted | Out-Null
    [System.IO.File]::WriteAllBytes($packagePath, $PackageBytes)
    $installedVersion = Get-InstalledVersion -Target $Target
    $rollback = $null
    $replacementStarted = $false
    try {
        Expand-AndValidatePackage -PackagePath $packagePath -Destination $extracted -VerifiedManifest $verified
        $rollback = New-RollbackArchive -Target $Target -VerifiedManifest $verified -StateDirectory $stateDirectory -InstalledVersion $installedVersion

        foreach ($file in $verified.Files) {
            if ($file.scope -eq "updater" -and $verified.UpdaterVersion -le $UpdaterVersion) {
                continue
            }
            $destinationDirectory = Get-DestinationDirectory -Target $Target -Scope $file.scope
            New-Item -ItemType Directory -Force -Path $destinationDirectory | Out-Null
            $source = Join-Path $extracted ("$($file.scope)\$($file.name)")
            $destination = Join-Path $destinationDirectory $file.name
            $temporary = "$destination.$([guid]::NewGuid().ToString('N')).new"
            Copy-Item -LiteralPath $source -Destination $temporary
            $replacementStarted = $true
            Move-Item -LiteralPath $temporary -Destination $destination -Force
        }

        $newVersion = Get-InstalledVersion -Target $Target
        if ($newVersion -ne $ExpectedVersion) {
            throw "The installed synth did not retain the expected version."
        }
        Remove-OldState -Product $product -CurrentBackup $rollback.Path
        return [pscustomobject]@{ Version = $newVersion; RollbackPath = $rollback.Path }
    }
    catch {
        if ($replacementStarted -and $rollback) {
            try {
                Restore-RollbackArchive -Target $Target -Rollback $rollback
            }
            catch {
                throw "Update failed and automatic rollback also failed. Close every audio host before restoring $($rollback.Path)."
            }
        }
        throw
    }
    finally {
        $null = Remove-DirectoryWithRetry -Path $workRoot
    }
}

function Invoke-TargetUpdate {
    param(
        [object] $Target,
        [object] $Release
    )

    $product = $Target.Product
    $stateDirectory = Get-ProductStateDirectory -Product $product
    $logDirectory = Join-Path $stateDirectory "logs"
    New-Item -ItemType Directory -Force -Path $logDirectory | Out-Null
    $logPath = Join-Path $logDirectory ("update-" + (Get-Date -Format "yyyy-MM-dd_HHmmss") + ".log")
    Write-UpdaterStatus "Downloading signed metadata for $($product.DisplayName) $($Release.VersionText)." -LogPath $logPath
    $manifestBytes = Get-HttpsBytes -Url $Release.ManifestUrl -MaximumBytes $MaximumMetadataBytes
    $signatureBytes = Get-HttpsBytes -Url $Release.SignatureUrl -MaximumBytes 64KB
    $signatureText = [System.Text.Encoding]::ASCII.GetString($signatureBytes)
    $verified = Read-AndVerifyManifest -ManifestBytes $manifestBytes -SignatureText $signatureText -ExpectedVersion $Release.Version -Product $product
    Write-UpdaterStatus "Downloading the verified runtime package." -LogPath $logPath
    $packageBytes = Get-HttpsBytes -Url $verified.PackageUrl -MaximumBytes $MaximumPackageBytes
    $result = Install-VerifiedUpdate -Target $Target -ManifestBytes $manifestBytes -SignatureText $signatureText -ExpectedVersion $Release.Version -PackageBytes $packageBytes
    Write-UpdaterStatus "$($product.DisplayName) $($result.Version.ToString(3)) was installed successfully." -LogPath $logPath -Always
    Write-UpdaterStatus "Rollback ZIP: $($result.RollbackPath)" -LogPath $logPath
    Remove-OldState -Product $product -CurrentBackup $result.RollbackPath
}

function Complete-Run {
    param([int] $ExitCode)

    if ($PauseWhenDone -and -not $NonInteractive -and -not [Console]::IsInputRedirected) {
        $null = Read-Host "Press Enter to close"
    }
    exit $ExitCode
}

if ($LibraryOnly) {
    return
}

try {
    Write-UpdaterStatus "Searching for installed Onj Research Yamaha hybrid synths."
    $targets = @(Find-SynthTargets)
    if ($targets.Count -eq 0) {
        Write-UpdaterStatus "No S-YXG100 Hybrid or S-YXG2026 Hybrid DLL was found within two folder levels of:`n$ResolvedSearchDirectory" -Always
        Complete-Run 2
    }

    Write-UpdaterStatus "Found $($targets.Count) installed synth folder(s):"
    foreach ($target in $targets) {
        Write-UpdaterStatus "  $($target.Product.DisplayName): $($target.RuntimeDirectory)"
    }

    $pending = New-Object System.Collections.Generic.List[object]
    $unpublishedProducts = 0
    foreach ($productGroup in @($targets | Group-Object { $_.Product.Id })) {
        $product = $productGroup.Group[0].Product
        Write-UpdaterStatus "Checking GitHub for $($product.DisplayName) updates."
        $release = Get-LatestProductRelease -Product $product
        if (-not $release) {
            Write-UpdaterStatus "$($product.DisplayName) has no published stable update yet."
            $unpublishedProducts++
            continue
        }
        foreach ($target in $productGroup.Group) {
            $installedVersion = Get-InstalledVersion -Target $target
            if ($release.Version -le $installedVersion) {
                Write-UpdaterStatus "$($product.DisplayName) $($installedVersion.ToString(3)) is up to date in $($target.RuntimeDirectory)."
                continue
            }
            Write-UpdaterStatus ""
            Write-UpdaterStatus "$($product.DisplayName) $($release.VersionText) is available for:"
            Write-UpdaterStatus "  $($target.RuntimeDirectory)"
            if (-not [string]::IsNullOrWhiteSpace($release.Notes)) {
                Write-UpdaterStatus "What's new:"
                Write-UpdaterStatus $release.Notes
            }
            $pending.Add([pscustomobject]@{ Target = $target; Release = $release })
        }
    }

    if ($pending.Count -eq 0) {
        if ($unpublishedProducts -gt 0) {
            Write-UpdaterStatus "No installable update was found. One or more detected synths do not yet have a published stable release." -Always
        }
        else {
            Write-UpdaterStatus "Every detected synth is up to date." -Always
        }
        Complete-Run 0
    }
    if ($CheckOnly) {
        Complete-Run 10
    }

    if (-not $Install) {
        if ($NonInteractive -or [Console]::IsInputRedirected) {
            Write-UpdaterStatus "Updates are available. Run Update Yamaha Hybrids.cmd to install them." -Always
            Complete-Run 10
        }
        Write-UpdaterStatus "1. Install all $($pending.Count) detected update(s)"
        Write-UpdaterStatus "2. Cancel"
        $answer = Read-Host "Choose 1 or 2"
        if ($answer -ne "1") {
            Write-UpdaterStatus "Update cancelled."
            Complete-Run 0
        }
    }

    foreach ($item in $pending) {
        Invoke-TargetUpdate -Target $item.Target -Release $item.Release
    }
    Write-UpdaterStatus "All selected Yamaha hybrid updates completed." -Always
    Complete-Run 0
}
catch {
    $message = $_.Exception.Message
    if ($message -match 'being used by another process|access.*denied|cannot access') {
        $message += " Close Foobar2000, REAPER, VSTHost, and any other program using the synth, then run the updater again."
    }
    Write-UpdaterStatus "Update failed: $message" -Always
    Complete-Run 2
}
