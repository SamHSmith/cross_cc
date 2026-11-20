# Native Windows entry layer: choose a compiler, then hand the build to C.
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$caller = (Get-Location).Path
$compiler = $env:CROSS_CC_COMPILER
$rest = @()
foreach ($arg in $args) {
    if ($arg.StartsWith('--compiler=')) {
        $compiler = $arg.Substring(11)
        if (!$compiler) { throw '--compiler needs an executable' }
    } elseif ($arg -eq '--compiler') {
        throw 'use --compiler=C:\path\to\cross_cc.exe'
    } else { $rest += $arg }
}
function File-Hash([string]$path) {
    return (Get-FileHash -Algorithm SHA256 -LiteralPath $path).Hash.ToLowerInvariant()
}
function Resolve-Compiler([string]$path) {
    if (!$path.Contains('\') -and !$path.Contains('/')) {
        $path = (Get-Command $path -CommandType Application -ErrorAction Stop).Source
    } elseif (![IO.Path]::IsPathRooted($path)) {
        $path = Join-Path $caller $path
    }
    for ($i = 0; $i -lt 40; $i++) {
        $item = Get-Item -LiteralPath $path -Force
        if (!$item.Target) { return $item.FullName }
        $target = @($item.Target)[0]
        if ([IO.Path]::IsPathRooted($target)) { $path = $target }
        else { $path = Join-Path $item.DirectoryName $target }
    }
    throw 'compiler symlink chain is too long'
}
if (!$compiler) {
    $found = Get-Command cross_cc -CommandType Application -ErrorAction SilentlyContinue
    if ($found) { $compiler = $found.Source }
}
if (!$compiler) {
    if ($env:PROCESSOR_ARCHITECTURE -ne 'AMD64' -and $env:PROCESSOR_ARCHITEW6432 -ne 'AMD64') {
        throw 'No Windows ARM64 backstop is checked in; supply --compiler='
    }
    $hostKey = 'windows-amd64'
    $name = "cross_cc-v1-$hostKey.zip"
    $archive = Join-Path $root "backstop/v1/$name"
    $expected = ''
    foreach ($line in Get-Content -LiteralPath (Join-Path $root 'backstop/v1/SHA256SUMS')) {
        $fields = $line -split '\s+'
        if ($fields.Count -eq 2 -and $fields[1] -eq $name) { $expected = $fields[0] }
    }
    if ($expected.Length -ne 64 -or (File-Hash $archive) -ne $expected) {
        throw "backstop archive checksum mismatch: $name"
    }
    $base = Join-Path $root '.cache/bootstrap/v1'
    $cache = Join-Path $base $hostKey
    $compiler = Join-Path $cache "cross_cc-$hostKey/cross_cc.exe"
    [IO.Directory]::CreateDirectory($base) | Out-Null
    if (Test-Path -LiteralPath $cache) {
        $archiveReceipt = Join-Path $cache 'archive.sha256'
        $compilerReceipt = Join-Path $cache 'compiler.sha256'
        if (!(Test-Path -LiteralPath $compiler -PathType Leaf) -or
            !(Test-Path -LiteralPath $archiveReceipt -PathType Leaf) -or
            !(Test-Path -LiteralPath $compilerReceipt -PathType Leaf) -or
            [IO.File]::ReadAllText($archiveReceipt).Trim() -ne $expected -or
            (File-Hash $compiler) -ne [IO.File]::ReadAllText($compilerReceipt).Trim()) {
            throw "incomplete or changed backstop cache; move $cache aside and retry"
        }
    } else {
        $lock = Join-Path $base "$hostKey.lock"
        New-Item -ItemType Directory -Path $lock -ErrorAction Stop | Out-Null
        $stage = Join-Path $base ("s" + [Guid]::NewGuid().ToString('N').Substring(0, 12))
        $stageOwned = $false
        try {
            # Windows PowerShell 5.1's ZIP extractor still has MAX_PATH limits.
            # The longest path in this pinned package is 111 characters.
            if ($stage.Length + 112 -ge 260) {
                throw 'checkout path is too long for Windows PowerShell ZIP extraction; use a shorter path or --compiler='
            }
            New-Item -ItemType Directory -Path $stage -ErrorAction Stop | Out-Null
            $stageOwned = $true
            Expand-Archive -LiteralPath $archive -DestinationPath $stage
            $extracted = Join-Path $stage "cross_cc-$hostKey"
            foreach ($entry in @('cross_cc.exe', 'lib/include', 'lib/libc', 'upstream.lock.json')) {
                if (!(Test-Path -LiteralPath (Join-Path $extracted $entry))) {
                    throw "incomplete backstop package: $entry"
                }
            }
            [IO.File]::WriteAllText((Join-Path $stage 'archive.sha256'), "$expected`n", [Text.Encoding]::ASCII)
            $binaryHash = File-Hash (Join-Path $extracted 'cross_cc.exe')
            [IO.File]::WriteAllText((Join-Path $stage 'compiler.sha256'), "$binaryHash`n", [Text.Encoding]::ASCII)
            [IO.Directory]::Move($stage, $cache)
        } finally {
            if ($stageOwned -and (Test-Path -LiteralPath $stage)) { Remove-Item -LiteralPath $stage -Recurse -Force }
            Remove-Item -LiteralPath $lock
        }
    }
    Write-Host "build: using v1 backstop $compiler"
}
$compiler = Resolve-Compiler $compiler
if (!(Test-Path -LiteralPath $compiler -PathType Leaf)) { throw "compiler not found: $compiler" }
$seedRoot = $env:CROSS_CC_SEED_ROOT
if (!$seedRoot) {
    $probe = Split-Path -Parent $compiler
    while ($probe) {
        if ((Test-Path -LiteralPath (Join-Path $probe 'upstream.lock.json')) -and
            (Test-Path -LiteralPath (Join-Path $probe 'lib/include')) -and
            (Test-Path -LiteralPath (Join-Path $probe 'lib/libc'))) { $seedRoot = $probe; break }
        $parent = Split-Path -Parent $probe
        if ($parent -eq $probe) { break }
        $probe = $parent
    }
}
if (!$seedRoot -or !(Test-Path -LiteralPath (Join-Path $seedRoot 'lib/include'))) {
    throw 'compiler resources not found; keep its package together or set CROSS_CC_SEED_ROOT'
}
$seedRoot = (Get-Item -LiteralPath $seedRoot).FullName
$env:CROSS_CC_SEED_ROOT = $seedRoot
$toolDir = Join-Path $root 'build/tools'
$tmp = Join-Path $root '.cache/tmp'
$cacheDir = Join-Path $root '.cache/bootstrap/compiler-cache'
foreach ($dir in @($toolDir, $tmp, $cacheDir)) { [IO.Directory]::CreateDirectory($dir) | Out-Null }
Write-Host "build: seed $compiler"
$names = @('CROSS_CC_ROOT', 'CROSS_CC_LIB_DIR', 'CROSS_CC_CACHE_DIR', 'TMPDIR', 'TMP', 'TEMP',
           'ZIG_LIB_DIR', 'ZIG_GLOBAL_CACHE_DIR', 'ZIG_LIBC', 'CPATH', 'C_INCLUDE_PATH', 'CPLUS_INCLUDE_PATH',
           'LIBRARY_PATH', 'COMPILER_PATH', 'GCC_EXEC_PREFIX', 'SDKROOT', 'MACOSX_DEPLOYMENT_TARGET',
           'CCC_OVERRIDE_OPTIONS', 'CL', '_CL_')
$saved = @{}
foreach ($name in $names) { $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process') }
try {
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name, $null, 'Process') }
    $env:CROSS_CC_ROOT = $seedRoot
    $env:CROSS_CC_LIB_DIR = Join-Path $seedRoot 'lib'
    $env:CROSS_CC_CACHE_DIR = $cacheDir
    $env:TMPDIR = $tmp; $env:TMP = $tmp; $env:TEMP = $tmp
    foreach ($tool in @('build', 'release', 'linux-release-deps', 'macos-x64-llvm')) {
        $source = Join-Path $root "tools/$tool.c"
        $part = Join-Path $toolDir "$tool.part.exe"
        # The launcher itself needs no Microsoft SDK/license: use bundled MinGW.
        $ccArgs = @('cc', '-target', 'x86_64-windows-gnu', '-std=c11', '-O1', '-g0', $source, '-o', $part)
        & $compiler @ccArgs
        if ($LASTEXITCODE -ne 0) { throw "compiling $tool failed: $LASTEXITCODE" }
        Move-Item -LiteralPath $part -Destination (Join-Path $toolDir "$tool.exe") -Force
    }
} finally {
    foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
}
if (!$rest.Count) { $rest = @('build') }
elseif ($rest[0].StartsWith('-') -and $rest[0] -ne '--help' -and $rest[0] -ne '-h') { $rest = @('build') + $rest }
Set-Location -LiteralPath $root
& (Join-Path $toolDir 'build.exe') @rest "--compiler=$compiler"
exit $LASTEXITCODE
