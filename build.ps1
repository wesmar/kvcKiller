# KvcKiller - deterministic Release x64 build script

[CmdletBinding()]
param(
    [switch]$Clean,
    [switch]$NoBuild
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ProjectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$ProjectFile = Join-Path $ProjectRoot "src\KvcKiller.vcxproj"
$OutputDir   = Join-Path $ProjectRoot "bin"
$OutputExe   = Join-Path $OutputDir "KvcKiller.exe"
$RscIcon     = Join-Path $ProjectRoot "rsc\kvc.ico"
$Driver1     = Join-Path $ProjectRoot "rsc\driver1.sys"
$Driver2     = Join-Path $ProjectRoot "rsc\driver2.sys"
$IconDir     = Join-Path $ProjectRoot "ICON"
$TargetIcon  = Join-Path $IconDir "kvc.ico"

$TransientDirs = @(
    (Join-Path $ProjectRoot ".vs"),
    (Join-Path $ProjectRoot "bin-test"),
    (Join-Path $ProjectRoot "obj"),
    (Join-Path $ProjectRoot "obj-test"),
    (Join-Path $ProjectRoot "x64"),
    (Join-Path $ProjectRoot "src\obj"),
    (Join-Path $ProjectRoot "src\x64"),
    (Join-Path $ProjectRoot "src\.vs")
)

function Resolve-MSBuild {
    # 1. Official Microsoft locator tool: vswhere (queries newest instance)
    $vswhereCandidates = @(
        (Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"),
        (Join-Path $env:ProgramFiles "Microsoft Visual Studio\Installer\vswhere.exe")
    )
    $vswhereCmd = Get-Command vswhere.exe -ErrorAction SilentlyContinue
    if ($vswhereCmd) {
        $vswhereCandidates += $vswhereCmd.Source
    }

    $vswhere = $vswhereCandidates | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -First 1
    if ($vswhere) {
        $installationPath = & $vswhere -latest -prerelease -products * -requires Microsoft.Component.MSBuild -property installationPath
        if ($LASTEXITCODE -eq 0 -and $installationPath) {
            $candidates = @(
                (Join-Path $installationPath "MSBuild\Current\Bin\MSBuild.exe"),
                (Join-Path $installationPath "MSBuild\Current\Bin\amd64\MSBuild.exe"),
                (Join-Path $installationPath "MSBuild\15.0\Bin\MSBuild.exe")
            )
            foreach ($candidate in $candidates) {
                if (Test-Path -LiteralPath $candidate) {
                    return $candidate
                }
            }
        }

        # Secondary search using vswhere -find
        $foundPaths = & $vswhere -latest -prerelease -products * -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\MSBuild.exe
        if ($LASTEXITCODE -eq 0 -and $foundPaths) {
            $first = ($foundPaths -split "\r?\n" | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -First 1)
            if ($first) {
                return $first
            }
        }
    }

    # 2. Filesystem search across standard Program Files roots, sorted descending by version
    $searchRoots = @(
        $env:ProgramFiles,
        ${env:ProgramFiles(x86)}
    ) | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -Unique

    $discoveredVsDirs = @()
    foreach ($root in $searchRoots) {
        $vsRoot = Join-Path $root "Microsoft Visual Studio"
        if (Test-Path -LiteralPath $vsRoot) {
            $discoveredVsDirs += Get-ChildItem -LiteralPath $vsRoot -Directory -ErrorAction SilentlyContinue
        }
    }

    # Sort descending so newest Visual Studio version (e.g. 18, 17...) is evaluated first
    $sortedVsDirs = $discoveredVsDirs | Sort-Object {
        $cleanNum = ($_.Name -replace '[^\d.]', '')
        if ($cleanNum) { [version]$cleanNum } else { [version]'0.0' }
    } -Descending

    $editions = @("Enterprise", "Professional", "Community", "BuildTools", "Preview")
    foreach ($dir in $sortedVsDirs) {
        foreach ($edition in $editions) {
            $candidates = @(
                (Join-Path $dir.FullName "$edition\MSBuild\Current\Bin\MSBuild.exe"),
                (Join-Path $dir.FullName "$edition\MSBuild\Current\Bin\amd64\MSBuild.exe"),
                (Join-Path $dir.FullName "$edition\MSBuild\15.0\Bin\MSBuild.exe")
            )
            foreach ($candidate in $candidates) {
                if (Test-Path -LiteralPath $candidate) {
                    return $candidate
                }
            }
        }
    }

    # 3. Environment PATH fallback
    $pathCmd = Get-Command msbuild.exe -ErrorAction SilentlyContinue
    if ($pathCmd) {
        return $pathCmd.Source
    }

    throw "MSBuild not found. Install Visual Studio or Build Tools with Desktop C++ workload."
}

function Build-IconPayload {
    param(
        [Parameter(Mandatory = $true)][string]$IconSource,
        [Parameter(Mandatory = $true)][string]$Driver1Source,
        [Parameter(Mandatory = $true)][string]$Driver2Source,
        [Parameter(Mandatory = $true)][string]$TargetIcon
    )

    if (-not (Test-Path -LiteralPath $IconSource)) {
        throw "Icon source file not found: $IconSource"
    }
    if (-not (Test-Path -LiteralPath $Driver1Source)) {
        throw "PPL unprotect driver not found: $Driver1Source"
    }
    if (-not (Test-Path -LiteralPath $Driver2Source)) {
        throw "Process termination driver not found: $Driver2Source"
    }

    $targetDir = Split-Path -Parent $TargetIcon
    if (-not (Test-Path -LiteralPath $targetDir)) {
        New-Item -ItemType Directory -Path $targetDir -Force | Out-Null
    }

    $tempDir = Join-Path ([System.IO.Path]::GetTempPath()) ("kvckiller_icon_" + [System.Guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Path $tempDir -Force | Out-Null

    try {
        $tempDriver1 = Join-Path $tempDir "driver1"
        $tempDriver2 = Join-Path $tempDir "driver2"
        Copy-Item -LiteralPath $Driver1Source -Destination $tempDriver1 -Force
        Copy-Item -LiteralPath $Driver2Source -Destination $tempDriver2 -Force

        # Keep cabinet metadata deterministic regardless of checkout timestamps.
        $cabTimestamp = [DateTime]"2030-01-01 00:00:00"
        foreach ($staged in @($tempDriver1, $tempDriver2)) {
            $item = Get-Item -LiteralPath $staged
            $item.CreationTime = $cabTimestamp
            $item.LastWriteTime = $cabTimestamp
            $item.LastAccessTime = $cabTimestamp
        }

        $ddf = @"
.Set CabinetNameTemplate=kvckiller.cab
.Set DiskDirectoryTemplate=$tempDir
.Set CompressionType=LZX
.Set CompressionMemory=21
.Set MaxCabinetSize=0
.Set Cabinet=on
.Set Compress=on
"$tempDriver1" driver1
"$tempDriver2" driver2
"@
        $ddfPath = Join-Path $tempDir "pack.ddf"
        [System.IO.File]::WriteAllText($ddfPath, $ddf, [System.Text.Encoding]::ASCII)

        Write-Host "[*] Compressing both kernel components into one CAB (LZX:21)..." -ForegroundColor Cyan
        $process = Start-Process makecab.exe `
            -ArgumentList '/F', "`"$ddfPath`"" `
            -Wait -PassThru -NoNewWindow -WorkingDirectory $tempDir
        if ($process.ExitCode -ne 0) {
            throw "makecab failed (exit code $($process.ExitCode))."
        }

        $tempCab = Join-Path $tempDir "kvckiller.cab"
        if (-not (Test-Path -LiteralPath $tempCab)) {
            throw "Cabinet was not produced: $tempCab"
        }

        Write-Host "[*] Assembling payload into: $TargetIcon" -ForegroundColor Cyan
        $iconBytes = [System.IO.File]::ReadAllBytes($IconSource)
        $cabBytes = [System.IO.File]::ReadAllBytes($tempCab)
        $fsOut = [System.IO.File]::Create($TargetIcon)
        try {
            $fsOut.Write($iconBytes, 0, $iconBytes.Length)
            $fsOut.Write($cabBytes, 0, $cabBytes.Length)
        }
        finally {
            $fsOut.Dispose()
        }

        $iconItem = Get-Item -LiteralPath $TargetIcon
        Write-Host ("[+] Generated icon payload: {0} ({1:N2} KB)" -f $TargetIcon, ($iconItem.Length / 1KB)) -ForegroundColor Green

        $headerPath = Join-Path $ProjectRoot "src\GenIconSize.h"
        $headerText = @"
// Generated by build.ps1. Do not edit manually.
#pragma once
#define ICON_HEADER_SIZE $($iconBytes.Length)
"@
        [System.IO.File]::WriteAllText($headerPath, $headerText, [System.Text.Encoding]::ASCII)
        Write-Host "[+] Cabinet offset: $($iconBytes.Length) bytes" -ForegroundColor Green
    }
    finally {
        if (Test-Path -LiteralPath $tempDir) {
            Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}

function Get-PresentTransientDirs {
    $present = @()
    foreach ($dir in $TransientDirs) {
        if (Test-Path -LiteralPath $dir) {
            $present += $dir
        }
    }
    return @($present)
}

function Remove-TransientDirs {
    $present = @(Get-PresentTransientDirs)
    if ($present.Length -gt 0) {
        Write-Host "[*] Removing transient build directories..." -ForegroundColor Yellow
        foreach ($dir in $present) {
            Remove-Item -LiteralPath $dir -Recurse -Force -ErrorAction SilentlyContinue
            Write-Host "  [-] Removed: $dir" -ForegroundColor DarkGray
        }
    }

    # Ensure no stray obj files remain anywhere in the project tree
    $strayObj = @(Get-ChildItem -LiteralPath $ProjectRoot -Recurse -Filter "*.obj" -File -ErrorAction SilentlyContinue)
    if ($strayObj.Length -gt 0) {
        foreach ($objFile in $strayObj) {
            Remove-Item -LiteralPath $objFile.FullName -Force -ErrorAction SilentlyContinue
        }
    }
}

function Assert-CleanLayout {
    $remaining = @(Get-PresentTransientDirs)
    if ($remaining.Length -gt 0) {
        throw ("Failed to clean build directories: " + ($remaining -join ", "))
    }
    $remainingObj = @(Get-ChildItem -LiteralPath $ProjectRoot -Recurse -Filter "*.obj" -File -ErrorAction SilentlyContinue)
    if ($remainingObj.Length -gt 0) {
        throw ("Transient obj files still present: " + (($remainingObj | Select-Object -ExpandProperty FullName) -join ", "))
    }
}

function Assert-ProjectExists {
    if (-not (Test-Path -LiteralPath $ProjectFile)) {
        throw "Project file not found: $ProjectFile"
    }
}

function Assert-OutputUnlocked {
    if (-not (Test-Path -LiteralPath $OutputExe)) {
        return
    }

    try {
        $stream = [System.IO.File]::Open($OutputExe,
            [System.IO.FileMode]::Open,
            [System.IO.FileAccess]::ReadWrite,
            [System.IO.FileShare]::None)
        $stream.Close()
    } catch {
        throw "Output executable is locked: $OutputExe. Close the running KvcKiller instance and retry."
    }
}

function Invoke-Build {
    param(
        [Parameter(Mandatory = $true)]
        [string]$MSBuildPath
    )

    if (-not (Test-Path -LiteralPath $OutputDir)) {
        New-Item -ItemType Directory -Path $OutputDir | Out-Null
    }

    $arguments = @(
        $ProjectFile
        "/t:Build"
        "/p:Configuration=Release;Platform=x64"
        "/m"
        "/nr:false"
        "/v:minimal"
    )

    Write-Host "[*] Building KvcKiller (Release x64)..." -ForegroundColor Cyan
    & $MSBuildPath @arguments
    if ($LASTEXITCODE -ne 0) {
        throw "MSBuild failed with exit code $LASTEXITCODE."
    }
}

try {
    Assert-ProjectExists
    $msbuild = Resolve-MSBuild
    Write-Host "[*] Using MSBuild: $msbuild" -ForegroundColor Cyan

    Remove-TransientDirs
    Assert-CleanLayout

    if ($Clean -or $NoBuild) {
        if ($Clean -and (Test-Path -LiteralPath $OutputExe)) {
            Remove-Item -LiteralPath $OutputExe -Force -ErrorAction SilentlyContinue
            Write-Host "  [-] Removed output: $OutputExe" -ForegroundColor DarkGray
        }
        Write-Host "[+] Clean completed." -ForegroundColor Green
        exit 0
    }

    # Generate ICON\kvc.ico from rsc\kvc.ico + CAB(driver1, driver2; LZX:21)
    Build-IconPayload -IconSource $RscIcon -Driver1Source $Driver1 `
        -Driver2Source $Driver2 -TargetIcon $TargetIcon

    Assert-OutputUnlocked
    Invoke-Build -MSBuildPath $msbuild

    # Purge obj and transient build outputs immediately after compilation
    Remove-TransientDirs
    Assert-CleanLayout

    if (-not (Test-Path -LiteralPath $OutputExe)) {
        throw "Build completed but output executable was not produced: $OutputExe"
    }

    $file = Get-Item -LiteralPath $OutputExe
    Write-Host "[+] Build successful." -ForegroundColor Green
    Write-Host "[+] Output: $($file.FullName)" -ForegroundColor Green
    Write-Host ("[+] Size: {0:N2} KB" -f ($file.Length / 1KB)) -ForegroundColor Green
    
    # Set deterministic timestamps for reproducibility and anti-forensics
    $timestamp = Get-Date "2030-01-01 00:00:00"
    $file.CreationTime = $timestamp
    $file.LastWriteTime = $timestamp
    Write-Host "[+] Timestamps set to: 2030-01-01 00:00:00" -ForegroundColor Gray
    
    exit 0
}
catch {
    Write-Host "[!] $_" -ForegroundColor Red
    exit 1
}
finally {
    if (-not $NoBuild -and -not $Clean) {
        Remove-TransientDirs
        try {
            Assert-CleanLayout
        } catch {
            Write-Warning "Clean layout check warning: $_"
        }
    }
}
