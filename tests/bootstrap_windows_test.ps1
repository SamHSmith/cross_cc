# Regression test for build.ps1 Resolve-Compiler (Windows PowerShell 5.1 compatible).
# Extracts the function via AST; does not run the bootstrap.
$ErrorActionPreference = 'Stop'
$checkout = Split-Path -Parent $PSScriptRoot
$tokens = $null
$parseErrors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile((Join-Path $checkout 'build.ps1'), [ref]$tokens, [ref]$parseErrors)
if ($parseErrors -and $parseErrors.Count -gt 0) {
    $parseErrors | ForEach-Object { Write-Host "PARSE ERROR: $($_.Message) at line $($_.Extent.StartLineNumber)" }
    throw 'build.ps1 has parse errors'
}
$fn = $ast.Find({ param($n) $n -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $n.Name -eq 'Resolve-Compiler' }, $true)
if (!$fn) { throw 'Resolve-Compiler not found in build.ps1' }
. ([scriptblock]::Create($fn.Extent.Text))

$fixture = Join-Path $checkout ("build/tests/resolve-" + [Guid]::NewGuid().ToString('N').Substring(0, 12))
[IO.Directory]::CreateDirectory($fixture) | Out-Null
$caller = $fixture
$failures = 0
function Check([string]$name, $actual, $expected) {
    if ($actual -ne $expected) { Write-Host "FAIL $name`n  got      $actual`n  expected $expected"; $script:failures++ }
    else { Write-Host "ok   $name" }
}
try {
    $real = Join-Path $fixture 'real.exe'
    [IO.File]::WriteAllText($real, 'x')
    $real = (Get-Item -LiteralPath $real).FullName
    Check 'plain file' (Resolve-Compiler $real) $real

    $hard = Join-Path $fixture 'hard.exe'
    New-Item -ItemType HardLink -Path $hard -Value $real | Out-Null
    $hard = (Get-Item -LiteralPath $hard).FullName
    Check 'hard link keeps its own path' (Resolve-Compiler $hard) $hard
    Check 'relative path uses caller' (Resolve-Compiler '.\hard.exe') $hard

    $canSym = $true
    $sub = Join-Path $fixture 'sub'
    [IO.Directory]::CreateDirectory($sub) | Out-Null
    try {
        New-Item -ItemType SymbolicLink -Path (Join-Path $sub 'rel.exe') -Value '..\real.exe' | Out-Null
    } catch { $canSym = $false; Write-Host "SKIP symlink tests: $($_.Exception.Message)" }
    if ($canSym) {
        Check 'relative symlink' (Resolve-Compiler (Join-Path $sub 'rel.exe')) $real
        New-Item -ItemType SymbolicLink -Path (Join-Path $fixture 'abs.exe') -Value $real | Out-Null
        Check 'absolute symlink' (Resolve-Compiler (Join-Path $fixture 'abs.exe')) $real
        New-Item -ItemType SymbolicLink -Path (Join-Path $fixture 'chain.exe') -Value 'abs.exe' | Out-Null
        Check 'symlink chain' (Resolve-Compiler (Join-Path $fixture 'chain.exe')) $real
        $a = Join-Path $fixture 'a.exe'
        $b = Join-Path $fixture 'b.exe'
        $cycleOk = $true
        try {
            [IO.File]::WriteAllText($b, 'x')
            New-Item -ItemType SymbolicLink -Path $a -Value 'b.exe' | Out-Null
            Remove-Item -LiteralPath $b -Force
            New-Item -ItemType SymbolicLink -Path $b -Value 'a.exe' | Out-Null
        } catch { $cycleOk = $false; Write-Host "SKIP cycle test: cannot build cycle: $($_.Exception.Message)" }
        if ($cycleOk) {
        $msg = ''
        try { Resolve-Compiler (Join-Path $fixture 'a.exe') | Out-Null } catch { $msg = $_.Exception.Message }
        Check 'symlink cycle' $msg 'compiler symlink chain is too long'
        }
    }
} finally {
    $full = [IO.Path]::GetFullPath($fixture)
    if ($full.StartsWith([IO.Path]::GetFullPath((Join-Path $checkout 'build/tests')) + [IO.Path]::DirectorySeparatorChar)) {
        Remove-Item -LiteralPath $full -Recurse -Force -ErrorAction SilentlyContinue
    }
}
if ($failures) { exit 1 }
Write-Host 'bootstrap_windows_test: pass'
