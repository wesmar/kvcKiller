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
$IconDir     = Join-Path $ProjectRoot "ICON"
$AppIcon     = Join-Path $IconDir "kvc.ico"
$GuestBmp    = Join-Path $IconDir "guest.bmp"

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
        if ($cleanNum) {
            if ($cleanNum -notmatch '\.') { $cleanNum = "$cleanNum.0" }
            [version]$cleanNum
        } else {
            [version]'0.0'
        }
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

function Resolve-Dumpbin {
    $vswhereCandidates = @(
        (Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"),
        (Join-Path $env:ProgramFiles "Microsoft Visual Studio\Installer\vswhere.exe")
    )
    $vswhere = $vswhereCandidates | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -First 1
    if ($vswhere) {
        $installationPath = & $vswhere -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($LASTEXITCODE -eq 0 -and $installationPath) {
            $dumpbin = Get-ChildItem "$installationPath\VC\Tools\MSVC" -Filter 'dumpbin.exe' -Recurse -ErrorAction SilentlyContinue |
                Where-Object { $_.FullName -match 'Hostx64\\x64' } |
                Select-Object -First 1 -ExpandProperty FullName
            if ($dumpbin) { return $dumpbin }
        }
    }
    $candidates = @(
        (Get-ChildItem "$env:ProgramFiles\Microsoft Visual Studio" -Filter 'dumpbin.exe' -Recurse -ErrorAction SilentlyContinue |
            Where-Object { $_.FullName -match 'Hostx64\\x64' } | Select-Object -First 1 -ExpandProperty FullName),
        (Get-Command dumpbin.exe -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source)
    )
    foreach ($c in $candidates) {
        if ($c -and (Test-Path -LiteralPath $c)) { return $c }
    }
    return $null
}

function Resolve-VCToolsVersion {
    $vswhereCandidates = @(
        (Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"),
        (Join-Path $env:ProgramFiles "Microsoft Visual Studio\Installer\vswhere.exe")
    )
    $vswhere = $vswhereCandidates | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -First 1
    if ($vswhere) {
        $installationPath = & $vswhere -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($LASTEXITCODE -eq 0 -and $installationPath) {
            $msvcDir = "$installationPath\VC\Tools\MSVC"
            if (Test-Path -LiteralPath $msvcDir) {
                $latest = Get-ChildItem -LiteralPath $msvcDir -Directory | Sort-Object Name -Descending | Select-Object -First 1
                if ($latest) { return $latest.Name }
            }
        }
    }
    return $null
}

function Test-NoCrtDependencies {
    param(
        [Parameter(Mandatory = $true)][string]$ExePath,
        [string]$DumpbinPath
    )

    Write-Host "[*] Verifying Zero-CRT imports (dumpbin)..." -ForegroundColor Cyan
    if (-not $DumpbinPath -or -not (Test-Path -LiteralPath $DumpbinPath)) {
        Write-Host "  [-] dumpbin.exe not found - skipping import verification." -ForegroundColor DarkGray
        return $true
    }

    $lines = & $DumpbinPath /dependents "$ExePath" 2>&1
    if (-not $lines) {
        Write-Host "  [-] dumpbin output not captured - skipping import verification." -ForegroundColor DarkGray
        return $true
    }

    $inSection = $false
    $deps = [System.Collections.Generic.List[string]]::new()
    foreach ($line in $lines) {
        if ($line -match 'Image has the following dependencies') { $inSection = $true; continue }
        if ($inSection) {
            if ($line -match '^\s*$' -and $deps.Count -gt 0) { break }
            $dll = $line.Trim()
            if ($dll -and $dll -notmatch '^Summary') { $deps.Add($dll) }
        }
    }

    if ($deps.Count -eq 0) {
        Write-Host "  [+] dumpbin: no external DLL dependencies." -ForegroundColor Green
        return $true
    }

    $forbidden = @('crt', 'msvcp', 'vcruntime', 'ucrtbase')
    $hasForbidden = $false

    Write-Host "  [*] Imported DLLs:" -ForegroundColor Cyan
    foreach ($d in $deps) {
        $bad = $false
        foreach ($f in $forbidden) {
            if ($d.ToLower().Contains($f)) { $bad = $true; break }
        }
        if ($bad) {
            Write-Host "    [!] $d (FORBIDDEN CRT DEPENDENCY!)" -ForegroundColor Red
            $hasForbidden = $true
        } else {
            Write-Host "    [+] $d" -ForegroundColor Green
        }
    }

    if ($hasForbidden) {
        throw "Zero-CRT check failed: CRT library found in import table!"
    }

    Write-Host "  [+] Zero-CRT verified: 100% native Windows OS APIs, 0 CRT dependencies!" -ForegroundColor Green
    return $true
}

function Ensure-Resources {
    param(
        [Parameter(Mandatory = $true)][string]$IconPath,
        [Parameter(Mandatory = $true)][string]$GuestBmpPath
    )

    if (-not (Test-Path -LiteralPath $IconPath)) {
        throw "Application icon not found: $IconPath"
    }
    if (-not (Test-Path -LiteralPath $GuestBmpPath)) {
        throw "Guest image not found: $GuestBmpPath"
    }

    $iconItem = Get-Item -LiteralPath $IconPath
    Write-Host ("[+] Application icon verified: {0} ({1:N2} KB)" -f $IconPath, ($iconItem.Length / 1KB)) -ForegroundColor Green

    $bmpItem = Get-Item -LiteralPath $GuestBmpPath
    Write-Host ("[+] Guest image verified: {0} ({1:N2} KB)" -f $GuestBmpPath, ($bmpItem.Length / 1KB)) -ForegroundColor Green

    $headerPath = Join-Path $ProjectRoot "src\GenIconSize.h"
    $headerText = @"
// Generated by build.ps1. Do not edit manually.
#pragma once
#define ICON_HEADER_SIZE 0
"@
    [System.IO.File]::WriteAllText($headerPath, $headerText, [System.Text.Encoding]::ASCII)
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
    Start-Sleep -Milliseconds 100
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
    )

    $vcToolsVer = Resolve-VCToolsVersion
    if ($vcToolsVer) {
        $arguments += "/p:VCToolsVersion=$vcToolsVer"
    }

    $arguments += @(
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
    $dumpbin = Resolve-Dumpbin
    if ($dumpbin) {
        Write-Host "[*] Using Dumpbin: $dumpbin" -ForegroundColor Cyan
    }

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

    # Verify ICON\guest.bmp and ICON\kvc.ico
    Ensure-Resources -IconPath $AppIcon -GuestBmpPath $GuestBmp

    Assert-OutputUnlocked
    Invoke-Build -MSBuildPath $msbuild

    # Purge obj and transient build outputs immediately after compilation
    Remove-TransientDirs
    Assert-CleanLayout

    if (-not (Test-Path -LiteralPath $OutputExe)) {
        throw "Build completed but output executable was not produced: $OutputExe"
    }

    Test-NoCrtDependencies -ExePath $OutputExe -DumpbinPath $dumpbin

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
