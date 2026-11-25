[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [string] $EngineRoot
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Test-EngineDirectory {
    param([string] $Directory)

    return $Directory -and
        (Test-Path -LiteralPath (Join-Path $Directory 'Engine\Build\BatchFiles\RunUAT.bat') -PathType Leaf) -and
        (Test-Path -LiteralPath (Join-Path $Directory 'Engine\Binaries\Win64\UnrealEditor.modules') -PathType Leaf)
}

function Find-EngineDirectory {
    param([string] $RequestedDirectory, [string] $CheckoutDirectory)

    if ($RequestedDirectory) {
        if (!(Test-EngineDirectory $RequestedDirectory)) {
            throw "No built Unreal Editor installation found at: $RequestedDirectory"
        }
        return (Resolve-Path -LiteralPath $RequestedDirectory).ProviderPath
    }

    for ($directory = [IO.DirectoryInfo]::new($CheckoutDirectory).Parent; $directory; $directory = $directory.Parent) {
        if ($directory.Name -eq 'Plugins' -and $directory.Parent -and $directory.Parent.Name -eq 'Engine' -and
            $directory.Parent.Parent -and
            (Test-EngineDirectory $directory.Parent.Parent.FullName)) {
            return $directory.Parent.Parent.FullName
        }
    }

    $registryKeys = @(
        'HKLM:\SOFTWARE\EpicGames\Unreal Engine\5.6',
        'HKLM:\SOFTWARE\WOW6432Node\EpicGames\Unreal Engine\5.6'
    )
    foreach ($key in $registryKeys) {
        $location = Get-ItemPropertyValue -LiteralPath $key -Name InstalledDirectory -ErrorAction SilentlyContinue
        if (Test-EngineDirectory $location) {
            return (Resolve-Path -LiteralPath $location).ProviderPath
        }
    }

    $launcherFile = Join-Path $env:ProgramData 'Epic\UnrealEngineLauncher\LauncherInstalled.dat'
    if (Test-Path -LiteralPath $launcherFile -PathType Leaf) {
        $launcher = Get-Content -Raw -LiteralPath $launcherFile | ConvertFrom-Json
        foreach ($installation in $launcher.InstallationList) {
            if ($installation.AppName -eq 'UE_5.6' -and (Test-EngineDirectory $installation.InstallLocation)) {
                return (Resolve-Path -LiteralPath $installation.InstallLocation).ProviderPath
            }
        }
    }

    throw 'Unreal Engine 5.6 was not detected. Run BuildAndInstall.bat "path to the Unreal Engine installation".'
}

function Find-PluginDescriptors {
    param([IO.DirectoryInfo] $Directory, [string] $Filename)

    if (!$Directory.Exists) {
        return
    }

    foreach ($child in $Directory.EnumerateDirectories()) {
        $descriptors = @($child.EnumerateFiles('*.uplugin'))
        if ($descriptors.Count) {
            $descriptors | Where-Object Name -EQ $Filename
        } elseif (!$child.Attributes.HasFlag([IO.FileAttributes]::ReparsePoint)) {
            Find-PluginDescriptors $child $Filename
        }
    }
}

function Get-Sha256 {
    param([string] $Path)

    $stream = [IO.File]::Open($Path, 'Open', 'Read', 'ReadWrite')
    try {
        $algorithm = [Security.Cryptography.SHA256]::Create()
        try {
            return [BitConverter]::ToString($algorithm.ComputeHash($stream)).Replace('-', '').ToLowerInvariant()
        } finally {
            $algorithm.Dispose()
        }
    } finally {
        $stream.Dispose()
    }
}

function Install-File {
    param(
        [string] $Source,
        [string] $Destination,
        [System.Collections.Generic.List[object]] $Changes
    )

    $parent = Split-Path -Path $Destination -Parent
    [void][IO.Directory]::CreateDirectory($parent)
    $temporary = Join-Path $parent ('.blueprint-diff-install-' + [guid]::NewGuid().ToString('N'))
    $backup = $null
    try {
        [IO.File]::Copy($Source, $temporary)
        if ([IO.File]::Exists($Destination)) {
            $backup = $temporary + '.backup'
            [IO.File]::Replace($temporary, $Destination, $backup)
        } else {
            [IO.File]::Move($temporary, $Destination)
        }
        $Changes.Add([pscustomobject]@{ Destination = $Destination; Backup = $backup })
    } finally {
        if ([IO.File]::Exists($temporary)) {
            [IO.File]::Delete($temporary)
        }
    }
}

$pluginName = 'BlueprintDiffEdit'
$descriptorName = "$pluginName.uplugin"
$repo = Split-Path -Path $PSScriptRoot -Parent
$tempRoot = (Resolve-Path -LiteralPath ([IO.Path]::GetTempPath())).ProviderPath.TrimEnd('\')
$buildRoot = $null

try {
    if (!$EngineRoot) {
        $EngineRoot = $env:UE_ROOT
    }
    $engine = Join-Path (Find-EngineDirectory $EngineRoot $repo) 'Engine'
    $pluginsRoot = Join-Path $engine 'Plugins'
    $installInPlace = $repo.StartsWith($pluginsRoot + '\', [StringComparison]::OrdinalIgnoreCase)
    $destination = if ($installInPlace) { $repo } else { Join-Path $pluginsRoot "Marketplace\$pluginName" }
    $destinationDescriptor = Join-Path $destination $descriptorName
    $conflicts = @(Find-PluginDescriptors $pluginsRoot $descriptorName |
        Where-Object FullName -NE $destinationDescriptor |
        Select-Object -ExpandProperty FullName)
    if ($conflicts.Count) {
        throw "Another $pluginName installation exists at $($conflicts -join ', '). Keep a single copy before installing to $destination."
    }
    $engineManifest = Get-Content -Raw -LiteralPath (Join-Path $engine 'Binaries\Win64\UnrealEditor.modules') | ConvertFrom-Json

    $buildRoot = Join-Path $tempRoot ('BlueprintDiffEditBuild-' + [guid]::NewGuid().ToString('N'))
    $inputPlugin = Join-Path $buildRoot $pluginName
    $package = Join-Path $buildRoot 'Package'
    [void][IO.Directory]::CreateDirectory($inputPlugin)
    foreach ($entry in $descriptorName, 'Source', 'Config', 'README.md', 'LICENSE') {
        Copy-Item -LiteralPath (Join-Path $repo $entry) -Destination $inputPlugin -Recurse
    }

    Write-Host "Building $pluginName for Win64 with $engine"
    $uat = Join-Path $engine 'Build\BatchFiles\RunUAT.bat'
    $descriptor = Join-Path $inputPlugin $descriptorName
    Push-Location -LiteralPath (Join-Path $engine 'Source')
    try {
        & $uat BuildPlugin "-Plugin=$descriptor" "-Package=$package" '-TargetPlatforms=Win64'
        if ($LASTEXITCODE -ne 0) {
            throw "Unreal BuildPlugin exited with code $LASTEXITCODE."
        }
    } finally {
        Pop-Location
    }

    if (!$installInPlace) {
        # BuildPlugin clears this flag. Preserve the checkout's default for the engine install.
        $sourceDescriptor = Get-Content -Raw -LiteralPath $descriptor | ConvertFrom-Json
        $packagedDescriptorPath = Join-Path $package $descriptorName
        $packagedDescriptor = Get-Content -Raw -LiteralPath $packagedDescriptorPath | ConvertFrom-Json
        Add-Member -InputObject $packagedDescriptor -NotePropertyName EnabledByDefault -NotePropertyValue $sourceDescriptor.EnabledByDefault -Force
        [IO.File]::WriteAllText($packagedDescriptorPath, ($packagedDescriptor | ConvertTo-Json -Depth 20) + [Environment]::NewLine)
    }

    $manifestRelative = 'Binaries\Win64\UnrealEditor.modules'
    $builtManifest = Get-Content -Raw -LiteralPath (Join-Path $package $manifestRelative) | ConvertFrom-Json
    if (!$builtManifest.BuildId -or $builtManifest.BuildId -cne $engineManifest.BuildId) {
        throw "BuildId mismatch. Engine: $($engineManifest.BuildId); package: $($builtManifest.BuildId)."
    }
    $moduleFilename = $builtManifest.Modules.$pluginName
    if (!$moduleFilename -or [IO.Path]::GetFileName($moduleFilename) -ne $moduleFilename -or
        [IO.Path]::GetExtension($moduleFilename) -ne '.dll') {
        throw "The build did not produce a Win64 module entry for $pluginName."
    }
    $moduleRelative = Join-Path 'Binaries\Win64' $moduleFilename
    $builtModule = Join-Path $package $moduleRelative
    $hash = Get-Sha256 $builtModule
    $installedModule = 'UnrealEditor-' + $pluginName + '-' + $hash.Substring(0, 12) + '.dll'
    $builtManifest.Modules.$pluginName = $installedModule
    $newManifest = Join-Path $buildRoot 'UnrealEditor.modules'
    [IO.File]::WriteAllText($newManifest, ($builtManifest | ConvertTo-Json -Depth 5) + [Environment]::NewLine)

    [void][IO.Directory]::CreateDirectory($destination)
    $lock = [IO.File]::Open((Join-Path $destination '.install.lock'), 'OpenOrCreate', 'ReadWrite', 'None')
    $changes = [System.Collections.Generic.List[object]]::new()
    try {
        foreach ($file in Get-ChildItem -LiteralPath $package -Recurse -File) {
            $relative = $file.FullName.Substring($package.Length + 1)
            if ($relative -eq $manifestRelative -or $file.Extension -eq '.pdb') {
                continue
            }
            if ($installInPlace -and $relative -notlike 'Binaries\*' -and $relative -notlike 'Intermediate\*') {
                continue
            }
            if ($relative -eq $moduleRelative) {
                $relative = Join-Path 'Binaries\Win64' $installedModule
            }
            $target = Join-Path $destination $relative
            if ((Test-Path -LiteralPath $target -PathType Leaf) -and
                ((Get-Sha256 $target) -eq (Get-Sha256 $file.FullName))) {
                continue
            }
            Install-File $file.FullName $target $changes
        }

        # The previous manifest remains usable until all new files are in place.
        Install-File $newManifest (Join-Path $destination $manifestRelative) $changes
    } catch {
        for ($i = $changes.Count - 1; $i -ge 0; --$i) {
            $change = $changes[$i]
            try {
                if ($change.Backup) {
                    [IO.File]::Replace($change.Backup, $change.Destination, [NullString]::Value)
                } else {
                    [IO.File]::Delete($change.Destination)
                }
            } catch {
                Write-Warning "Restore failed for $($change.Destination). Backup: $($change.Backup). $($_.Exception.Message)"
            }
        }
        throw
    } finally {
        $lock.Dispose()
    }

    foreach ($change in $changes) {
        if ($change.Backup) {
            Remove-Item -LiteralPath $change.Backup -ErrorAction Continue
        }
    }
    Write-Host "Installed $pluginName to $destination"
    Write-Host "Editor module: $installedModule"
    Write-Host 'Restart any open Unreal Editor to load the new build.'
} catch {
    [Console]::Error.WriteLine($_.Exception.Message)
    exit 1
} finally {
    if ($buildRoot -and (Test-Path -LiteralPath $buildRoot -PathType Container)) {
        $resolved = Get-Item -LiteralPath $buildRoot
        if ($resolved.Parent.FullName -eq $tempRoot -and $resolved.Name.StartsWith('BlueprintDiffEditBuild-')) {
            Remove-Item -LiteralPath $resolved.FullName -Recurse -Force -ErrorAction Continue
        }
    }
}
