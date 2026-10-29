param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug',
    [ValidateSet('all', 'common', 'game')][string]$Suite = 'all',
    [ValidateRange(1, 1000)][int]$Repeat = 10,
    [switch]$Asan,
    [switch]$SkipBuild,
    [string]$ToolsVersion = ''
)

$ErrorActionPreference = 'Stop'
$repoDirectory = Split-Path -Parent $PSScriptRoot
$outputConfiguration = if ($Asan) { "${Configuration}ASan" } else { $Configuration }
$runner = Join-Path $repoDirectory "x64\$outputConfiguration\LifetimeTests.exe"

if (-not $SkipBuild) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Installer (vswhere.exe) was not found.' }
    $installation = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -property installationPath
    if (-not $installation) { throw 'Visual Studio C++ build tools were not found.' }
    $msbuild = Join-Path $installation 'MSBuild\Current\Bin\MSBuild.exe'
    $buildArguments = @(
        (Join-Path $PSScriptRoot 'LifetimeTests.vcxproj'),
        '/t:Build', "/p:Configuration=$Configuration", '/p:Platform=x64',
        '/p:PreferredToolArchitecture=x64', '/m', '/v:minimal', '/nologo'
    )
    if ($ToolsVersion) { $buildArguments += "/p:VCToolsVersion=$ToolsVersion" }
    if ($Asan) { $buildArguments += '/p:EnableASAN=true' }
    & $msbuild @buildArguments
    if ($LASTEXITCODE -ne 0) { throw "LifetimeTests build failed: $LASTEXITCODE" }
}
if (-not (Test-Path -LiteralPath $runner)) { throw "Build the test runner first: $runner" }

# ASan's runtime is supplied by the selected MSVC installation.
if ($Asan) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $installation = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -property installationPath
    $toolsRoot = Join-Path $installation 'VC\Tools\MSVC'
    $selectedVersion = if ($ToolsVersion) { $ToolsVersion } else {
        (Get-ChildItem -LiteralPath $toolsRoot -Directory | Sort-Object Name -Descending | Select-Object -First 1).Name
    }
    $env:PATH = (Join-Path $toolsRoot "$selectedVersion\bin\Hostx64\x64") + ';' + $env:PATH
}

$caseIds = @(& $runner --list --suite $Suite)
if ($LASTEXITCODE -ne 0 -or $caseIds.Count -eq 0) { throw 'Cannot list scenarios from the runner.' }
$commonIds = @(& $runner --list --suite common)
if ($LASTEXITCODE -ne 0) { throw 'Cannot read scenario groups from the runner.' }
$results = Join-Path (Split-Path -Parent $runner) ("results\cli-$Suite-" + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $results -Force | Out-Null
$records = @()
foreach ($caseId in $caseIds) {
    if ($caseId -notmatch '^[a-z_]+$') { throw "Unexpected scenario ID: $caseId" }
    $stdout = Join-Path $results "$caseId.log"
    $stderr = Join-Path $results "$caseId.stderr.log"
    $started = [Diagnostics.Stopwatch]::StartNew()
    $process = Start-Process -FilePath $runner -ArgumentList @('--case', $caseId, '--repeat', "$Repeat") `
        -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    $timeout = [Math]::Max(30000, $Repeat * 20000 + 10000)
    $finished = $process.WaitForExit($timeout)
    if (-not $finished) {
        $process.Kill()
        $process.WaitForExit()
        $exitCode = 3
    } else {
        $process.WaitForExit()
        $exitCode = $process.ExitCode
    }
    $started.Stop()
    $status = if ($exitCode -eq 0) { 'PASS' } elseif ($exitCode -eq 3) { 'TIMEOUT' } else { 'FAIL' }
    $record = [pscustomobject]@{
        Group = $(if ($caseId -in $commonIds) { 'common' } else { 'game' })
        Case = $caseId; Status = $status; Repeat = $Repeat
        ExitCode = $exitCode; ElapsedMs = $started.ElapsedMilliseconds
    }
    $records += $record
    Write-Host "$status $caseId (repeat=$Repeat, $($started.ElapsedMilliseconds) ms)"
    if ($exitCode -ne 0) {
        Get-Content -LiteralPath $stdout -Tail 8
        Get-Content -LiteralPath $stderr -Tail 8
    }
    $process.Dispose()
}
$records | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $results 'summary.json') -Encoding UTF8
$groupSummary = @(
    foreach ($group in @('common', 'game')) {
        $groupRecords = @($records | Where-Object Group -eq $group)
        [pscustomobject]@{
            Group = $group
            Cases = $groupRecords.Count
            Passed = @($groupRecords | Where-Object Status -eq 'PASS').Count
            Failed = @($groupRecords | Where-Object Status -ne 'PASS').Count
            Iterations = $(if ($groupRecords.Count) { ($groupRecords | Measure-Object Repeat -Sum).Sum } else { 0 })
        }
    }
)
$groupSummary | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $results 'groups.json') -Encoding UTF8
Write-Host "Results: $results"
if (@($records | Where-Object { $_.Status -ne 'PASS' }).Count -ne 0) { exit 1 }
exit 0
