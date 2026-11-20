[CmdletBinding()]
param([switch]$BuildGui)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$project = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$parent = Join-Path $project '.verify'
$run = Join-Path $parent ('programming-' + [guid]::NewGuid().ToString('N'))
$evidence = Join-Path $project 'docs/internal/programming-contracts-latest.json'

function Assert-Boundary([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path)
    if (!$full.StartsWith($project + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Outside project boundary.' }
    for ($cursor = $full; $cursor; $cursor = [IO.Path]::GetDirectoryName($cursor)) {
        if ((Test-Path -LiteralPath $cursor) -and ((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw 'Reparse path is not permitted.'
        }
    }
}

Assert-Boundary $run
if (Test-Path -LiteralPath $run) { throw 'Run already exists.' }
[void][IO.Directory]::CreateDirectory($run)
$protected = @('build/SimpleProgrammingTool.exe', 'build/simple_programming_tool.ini',
               'src/build_info.generated.h', 'src/simple_programming_tool.rc', 'src/tool_build_identity.h')
$before = @{}
foreach ($relative in $protected) {
    $path = Join-Path $project $relative
    if (Test-Path -LiteralPath $path) { $before[$relative] = (Get-FileHash -LiteralPath $path).Hash }
}
$summary = [ordered]@{ scope = 'synthetic PC transport and isolated x64 builds'; started = [DateTimeOffset]::Now.ToString('o'); passed = $false; cleanup = $false; results = @(); hardware = 'NOT RUN'; privateFixtures = $false; byteBudget = 512MB }
$failure = $null
try {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $msbuild = @(& $vswhere -latest -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe')[0]
    if (!$msbuild) { throw 'MSBuild unavailable.' }
    $sdk = Get-ChildItem -LiteralPath (Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10/Include') -Directory |
        Where-Object { $_.Name -match '^10\.0\.\d+\.0$' } | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
    if (!$sdk) { throw 'Installed Windows SDK unavailable.' }
    $sdkArgument = "/p:_LatestWindowsTargetPlatformVersion=$($sdk.Name)"
    $sdkRootArgument = '/p:TargetPlatformSdkRootOverride=' + (Join-Path ${env:ProgramFiles(x86)} 'Windows Kits/10')
    $sdkDisplayArgument = '/p:TargetPlatformDisplayName=Windows SDK'
    foreach ($configuration in @('Release', 'Debug')) {
        $out = Join-Path $run ($configuration + '\')
        $obj = Join-Path $run ($configuration + '-obj\')
        & $msbuild (Join-Path $project 'tests/ProgrammingContracts.vcxproj') /m /nologo /v:minimal $sdkArgument $sdkRootArgument $sdkDisplayArgument "/p:Configuration=$configuration" /p:Platform=x64 "/p:WindowsTargetPlatformVersion=$($sdk.Name)" "/p:OutDir=$out" "/p:IntDir=$obj"
        if ($LASTEXITCODE -ne 0) { throw "$configuration synthetic build failed." }
        $fixture = Join-Path $run ('fixtures-' + $configuration)
        [void][IO.Directory]::CreateDirectory($fixture)
        $result = & (Join-Path $out 'ProgrammingContracts.exe') $fixture
        if ($LASTEXITCODE -ne 0) { throw "$configuration contracts failed: $result" }
        Write-Output $result
        $summary.results += "$configuration $result"
        if ($BuildGui) {
            # Preserve formal EXE/INI and the allocated product version. No GUI/device launch.
            $guiOut = Join-Path $run ('gui-' + $configuration + '\')
            $guiObj = Join-Path $run ('gui-' + $configuration + '-obj\')
            & $msbuild (Join-Path $project 'SimpleProgrammingTool.vcxproj') /m /nologo /v:minimal $sdkArgument $sdkRootArgument $sdkDisplayArgument "/p:Configuration=$configuration" /p:Platform=x64 "/p:WindowsTargetPlatformVersion=$($sdk.Name)" "/p:SolutionDir=$project\" "/p:OutDir=$guiOut" "/p:IntDir=$guiObj" /p:PreBuildEventUseInBuild=false
            if ($LASTEXITCODE -ne 0) { throw "$configuration GUI build failed." }
            $summary.results += "$configuration isolated GUI build PASS"
        }
    }
    $summary.passed = $true
} catch { $failure = $_; $summary.results += $_.ToString() }
finally {
    # Fresh producer-exclusive output tree; freeze exact identities before retirement.
    # Unknown changes or links stop retirement rather than widening ownership.
    $files = @(Get-ChildItem -LiteralPath $run -Recurse -File -Force)
    $manifest = @($files | ForEach-Object { Assert-Boundary $_.FullName; [pscustomobject]@{ Path = $_.FullName; Hash = (Get-FileHash -LiteralPath $_.FullName).Hash; Bytes = $_.Length } })
    $summary.generatedBytes = 0L
    foreach ($item in $manifest) { $summary.generatedBytes += $item.Bytes }
    foreach ($item in @(Get-ChildItem -LiteralPath $run -Recurse -Force)) { Assert-Boundary $item.FullName }
    foreach ($item in $manifest) {
        if ((Get-FileHash -LiteralPath $item.Path).Hash -ne $item.Hash) { throw 'Changed output prevents retirement.' }
    }
    foreach ($item in $manifest) { Remove-Item -LiteralPath $item.Path -ErrorAction Stop }
    foreach ($dir in @(Get-ChildItem -LiteralPath $run -Directory -Recurse | Sort-Object { $_.FullName.Length } -Descending)) {
        if (@(Get-ChildItem -LiteralPath $dir.FullName -Force).Count -eq 0) { Remove-Item -LiteralPath $dir.FullName -ErrorAction Stop }
    }
    if (@(Get-ChildItem -LiteralPath $run -Force).Count -eq 0) { Remove-Item -LiteralPath $run -ErrorAction Stop }
    $summary.cleanup = !(Test-Path -LiteralPath $run)
    foreach ($relative in $before.Keys) {
        if ((Get-FileHash -LiteralPath (Join-Path $project $relative)).Hash -ne $before[$relative]) { throw "Protected file changed: $relative" }
    }
    $summary.protectedHashesUnchanged = $true
    $summary | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $evidence -Encoding UTF8
}
if ($failure) { throw $failure }
if (!$summary.cleanup) { throw "Owned outputs remain: $run" }
