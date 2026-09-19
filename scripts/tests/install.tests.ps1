#Requires -Version 5.1
<#
    install.ps1 的 PATH 纯逻辑、清单校验与资源所有权判定验证脚本。

    不进 ctest,手工跑(CI 里由 ci.yml 的 install-scripts 腿跑):
        powershell -NoProfile -File scripts\tests\install.tests.ps1

    不碰真实 HKCU 注册表——通过点号(.)dot-source install.ps1,脚本内部靠
    "$MyInvocation.InvocationName -ne '.'" 这个判断,dot-source 时不会触发真正的安装动作。
    涉及 exit 3(needs-review)的整脚本路径另起子进程跑,断言退出码。
#>

$ErrorActionPreference = 'Stop'

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$installScript = Join-Path (Split-Path -Parent $scriptDir) 'install.ps1'

if (-not (Test-Path -LiteralPath $installScript)) {
    throw "找不到 install.ps1:$installScript"
}

# 点号调用:只加载函数定义,不会跑安装流程(见 install.ps1 末尾的守卫判断)
. $installScript

$script:passCount = 0
$script:failCount = 0

function Assert-Equal {
    param(
        [string]$Name,
        $Expected,
        $Actual
    )
    if ($Expected -eq $Actual) {
        Write-Host "[PASS] $Name" -ForegroundColor Green
        $script:passCount++
    } else {
        Write-Host "[FAIL] $Name" -ForegroundColor Red
        Write-Host "       期望: $(if ($null -eq $Expected) { '<null>' } else { $Expected })"
        Write-Host "       实际: $(if ($null -eq $Actual) { '<null>' } else { $Actual })"
        $script:failCount++
    }
}

function Assert-True {
    param([string]$Name, $Actual)
    Assert-Equal -Name $Name -Expected $true -Actual ([bool]$Actual)
}

function Assert-FileText {
    param([string]$Name, [string]$Path, [string]$ExpectedText)
    $actual = $null
    if (Test-Path -LiteralPath $Path -PathType Leaf) {
        # Set-Content 会落 CRLF 尾巴,比对前掐掉,只认正文
        $actual = ((Get-Content -LiteralPath $Path -Raw) -as [string]).TrimEnd("`r", "`n")
    }
    Assert-Equal -Name $Name -Expected $ExpectedText -Actual $actual
}

Write-Host "==== Get-UpdatedPathForAdd ====" -ForegroundColor Cyan

# 空 PATH 加一个目录 -> 直接就是那个目录
Assert-Equal -Name '空 PATH 追加' `
    -Expected 'C:\Tools\lubancode' `
    -Actual (Get-UpdatedPathForAdd -OldValue '' -NewDir 'C:\Tools\lubancode')

# 非空 PATH 追加 -> 用分号接在后面
Assert-Equal -Name '非空 PATH 追加' `
    -Expected 'C:\A;C:\B;C:\Tools\lubancode' `
    -Actual (Get-UpdatedPathForAdd -OldValue 'C:\A;C:\B' -NewDir 'C:\Tools\lubancode')

# 旧值结尾带多余分号 -> 不产生双分号
Assert-Equal -Name '旧值结尾带分号不产生双分号' `
    -Expected 'C:\A;C:\B;C:\Tools\lubancode' `
    -Actual (Get-UpdatedPathForAdd -OldValue 'C:\A;C:\B;' -NewDir 'C:\Tools\lubancode')

# 已包含(大小写不同)-> 返回 $null,不重复添加
Assert-Equal -Name '已包含时大小写不敏感去重' `
    -Expected $null `
    -Actual (Get-UpdatedPathForAdd -OldValue 'C:\A;c:\tools\lubancode;C:\B' -NewDir 'C:\Tools\lubancode')

# 已包含但结尾多个反斜杠 -> 依然识别为重复
Assert-Equal -Name '已包含时忽略结尾反斜杠差异' `
    -Expected $null `
    -Actual (Get-UpdatedPathForAdd -OldValue 'C:\A;C:\Tools\lubancode\;C:\B' -NewDir 'C:\Tools\lubancode')

# 旧值里含未展开的 %VAR%,只做字符串追加,不能被展开或破坏
Assert-Equal -Name '保留旧值里未展开的 %VAR% 字面量' `
    -Expected '%SystemRoot%\system32;C:\Tools\lubancode' `
    -Actual (Get-UpdatedPathForAdd -OldValue '%SystemRoot%\system32' -NewDir 'C:\Tools\lubancode')

Write-Host ""
Write-Host "==== Add-DirToProcessPath ====" -ForegroundColor Cyan

$savedProcessPath = $env:Path
try {
    $env:Path = 'C:\A;C:\B'

    Assert-True -Name '当前进程缺目录时追加并报告改动' `
        -Actual (Add-DirToProcessPath -Dir 'C:\Tools\lubancode')
    Assert-Equal -Name '当前进程 PATH 已立即更新' `
        -Expected 'C:\A;C:\B;C:\Tools\lubancode' `
        -Actual $env:Path
    Assert-Equal -Name '当前进程已有目录时不重复追加' -Expected $false `
        -Actual (Add-DirToProcessPath -Dir 'c:\tools\lubancode\')
    Assert-Equal -Name '重复追加后 PATH 保持不变' `
        -Expected 'C:\A;C:\B;C:\Tools\lubancode' `
        -Actual $env:Path
} finally {
    $env:Path = $savedProcessPath
}

Write-Host ""
Write-Host "==== Get-UpdatedPathForRemove ====" -ForegroundColor Cyan

# 目录在中间 -> 摘除后两边保留,不留双分号
Assert-Equal -Name '摘除中间目录' `
    -Expected 'C:\A;C:\B' `
    -Actual (Get-UpdatedPathForRemove -OldValue 'C:\A;C:\Tools\lubancode;C:\B' -DirToRemove 'C:\Tools\lubancode')

# 目录在末尾
Assert-Equal -Name '摘除末尾目录' `
    -Expected 'C:\A;C:\B' `
    -Actual (Get-UpdatedPathForRemove -OldValue 'C:\A;C:\B;C:\Tools\lubancode' -DirToRemove 'C:\Tools\lubancode')

# 目录不存在 -> 原样返回旧值(调用方靠字符串相等判断"未改动")
Assert-Equal -Name '目录不存在时原样返回' `
    -Expected 'C:\A;C:\B' `
    -Actual (Get-UpdatedPathForRemove -OldValue 'C:\A;C:\B' -DirToRemove 'C:\Tools\lubancode')

# 大小写、结尾反斜杠差异也要能摘干净
Assert-Equal -Name '摘除时大小写/结尾反斜杠不敏感' `
    -Expected 'C:\A;C:\B' `
    -Actual (Get-UpdatedPathForRemove -OldValue 'C:\A;c:\tools\lubancode\;C:\B' -DirToRemove 'C:\Tools\lubancode')

# 只剩这一个目录 -> 摘除后是空字符串
Assert-Equal -Name '摘完最后一个目录得到空字符串' `
    -Expected '' `
    -Actual (Get-UpdatedPathForRemove -OldValue 'C:\Tools\lubancode' -DirToRemove 'C:\Tools\lubancode')

Write-Host ""
Write-Host "==== Get-DefaultInstallDir ====" -ForegroundColor Cyan

# Linux/macOS 的 pwsh 没有 LOCALAPPDATA,补个假值再验拼接语义
$savedLocalAppData = $env:LOCALAPPDATA
if (-not $savedLocalAppData) { $env:LOCALAPPDATA = (Join-Path ([IO.Path]::GetTempPath()) 'fake-localappdata') }
try {
    Assert-Equal -Name '默认安装目录拼接正确' `
        -Expected (Join-Path $env:LOCALAPPDATA 'Programs\lubancode') `
        -Actual (Get-DefaultInstallDir)
} finally {
    if (-not $savedLocalAppData) { Remove-Item Env:\LOCALAPPDATA -ErrorAction SilentlyContinue }
}

# =====================================================================
# 清单路径规则(GitHubRelease自动更新单 §四):拒绝绝对路径/盘符/UNC/..
# ADS/保留名/结尾点空格/非法字符;大小写折叠碰撞在整体校验里挡。
# =====================================================================
Write-Host ""
Write-Host "==== Test-ManifestRelativePath ====" -ForegroundColor Cyan

Assert-True -Name '正常相对路径放行' -Actual (Test-ManifestRelativePath -RelPath 'skills/lubancode-config/SKILL.md')
Assert-True -Name '中文文件名放行' -Actual (Test-ManifestRelativePath -RelPath 'docs/中文指南.md')
Assert-True -Name '大小写混合放行' -Actual (Test-ManifestRelativePath -RelPath 'Skills/README.md')

Assert-Equal -Name '拒绝空路径' -Expected $false -Actual (Test-ManifestRelativePath -RelPath '')
Assert-Equal -Name '拒绝绝对路径' -Expected $false -Actual (Test-ManifestRelativePath -RelPath '/etc/passwd')
Assert-Equal -Name '拒绝反斜杠' -Expected $false -Actual (Test-ManifestRelativePath -RelPath 'a\b.md')
Assert-Equal -Name '拒绝盘符冒号' -Expected $false -Actual (Test-ManifestRelativePath -RelPath 'C:/x.md')
Assert-Equal -Name '拒绝 ADS 冒号' -Expected $false -Actual (Test-ManifestRelativePath -RelPath 'skills/a.md:stream')
Assert-Equal -Name '拒绝 UNC 开头' -Expected $false -Actual (Test-ManifestRelativePath -RelPath '//server/share')
Assert-Equal -Name '拒绝上跳段' -Expected $false -Actual (Test-ManifestRelativePath -RelPath 'skills/../secrets')
Assert-Equal -Name '拒绝当前段' -Expected $false -Actual (Test-ManifestRelativePath -RelPath './skills/a.md')
Assert-Equal -Name '拒绝空段' -Expected $false -Actual (Test-ManifestRelativePath -RelPath 'skills//a.md')
Assert-Equal -Name '拒绝结尾点' -Expected $false -Actual (Test-ManifestRelativePath -RelPath 'skills/a.md.')
Assert-Equal -Name '拒绝结尾空格' -Expected $false -Actual (Test-ManifestRelativePath -RelPath 'skills/a.md ')
Assert-Equal -Name '拒绝保留名 CON' -Expected $false -Actual (Test-ManifestRelativePath -RelPath 'docs/CON')
Assert-Equal -Name '拒绝保留名 com1 小写' -Expected $false -Actual (Test-ManifestRelativePath -RelPath 'docs/com1.md')
Assert-Equal -Name '拒绝非法字符 *' -Expected $false -Actual (Test-ManifestRelativePath -RelPath 'docs/a*b.md')
Assert-Equal -Name '拒绝超长路径' -Expected $false -Actual (Test-ManifestRelativePath -RelPath ('a/' * 300 + 'x.md'))

# =====================================================================
# 清单整体校验:大小写碰撞、sha 形状、schema/algo。
# =====================================================================
Write-Host ""
Write-Host "==== Test-ManifestObject ====" -ForegroundColor Cyan

$shaA = 'a' * 64
$shaB = 'b' * 64
$goodManifest = [PSCustomObject]@{
    schema = 1; algo = 'sha256'; file_count = 2
    files = @(
        [PSCustomObject]@{ path = 'skills/a.md'; sha256 = $shaA }
        [PSCustomObject]@{ path = 'docs/b.md'; sha256 = $shaB }
    )
}
Assert-Equal -Name '合格清单零问题' `
    -Expected 0 -Actual (@(Test-ManifestObject -Manifest $goodManifest -Where 't').Count)

$caseClash = [PSCustomObject]@{
    schema = 1; algo = 'sha256'; file_count = 2
    files = @(
        [PSCustomObject]@{ path = 'skills/a.md'; sha256 = $shaA }
        [PSCustomObject]@{ path = 'Skills/A.md'; sha256 = $shaB }
    )
}
Assert-True -Name '大小写碰撞被查出' `
    -Actual ((Test-ManifestObject -Manifest $caseClash -Where 't') -join '; ').Contains('大小写碰撞')

$badSha = [PSCustomObject]@{
    schema = 1; algo = 'sha256'; file_count = 1
    files = @([PSCustomObject]@{ path = 'a.md'; sha256 = 'XYZ' })
}
Assert-True -Name '坏 sha256 被查出' `
    -Actual ((Test-ManifestObject -Manifest $badSha -Where 't') -join '; ').Contains('sha256')

$badSchema = [PSCustomObject]@{ schema = 99; algo = 'sha256'; files = @() }
Assert-True -Name '坏 schema 被查出' `
    -Actual ((Test-ManifestObject -Manifest $badSchema -Where 't') -join '; ').Contains('schema')

# =====================================================================
# 决策表(GitHubRelease自动更新单 §四):合成三张表,不打文件系统。
# =====================================================================
Write-Host ""
Write-Host "==== Build-FilePlan 决策表 ====" -ForegroundColor Cyan

function New-MapEntry {
    param([string]$Path, [string]$Sha)
    return @{ path = $Path; sha256 = $Sha }
}

function New-DiskEntry {
    param([string]$Path, [string]$Sha, [switch]$IsDir, [switch]$Reparse)
    return @{
        path = $Path; sha256 = $Sha; isDir = [bool]$IsDir; reparse = [bool]$Reparse
        # Linux/macOS 的 pwsh 没有 $env:TEMP,用平台临时目录
        abspath = Join-Path ([IO.Path]::GetTempPath()) ($Path -replace '/', [IO.Path]::DirectorySeparatorChar)
    }
}

$h0 = '0' * 64; $h1 = '1' * 64; $h2 = '2' * 64; $h9 = '9' * 64
$newMap = @{
    'skills/replace.md'    = (New-MapEntry 'skills/replace.md' $h1)
    'skills/same.md'       = (New-MapEntry 'skills/same.md' $h2)
    'skills/new.md'        = (New-MapEntry 'skills/new.md' $h1)
    'skills/modified.md'   = (New-MapEntry 'skills/modified.md' $h1)
    'skills/collide.md'    = (New-MapEntry 'skills/collide.md' $h1)
    'skills/missing.md'    = (New-MapEntry 'skills/missing.md' $h1)
}
$oldMap = @{
    'skills/replace.md'    = (New-MapEntry 'skills/replace.md' $h0)
    'skills/same.md'       = (New-MapEntry 'skills/same.md' $h2)
    'skills/modified.md'   = (New-MapEntry 'skills/modified.md' $h0)
    'skills/missing.md'    = (New-MapEntry 'skills/missing.md' $h0)
    'skills/retire.md'     = (New-MapEntry 'skills/retire.md' $h0)
    'skills/mod-retired.md' = (New-MapEntry 'skills/mod-retired.md' $h0)
    'skills/ghost.md'      = (New-MapEntry 'skills/ghost.md' $h0)
    'skills/dir-where-file.md' = (New-MapEntry 'skills/dir-where-file.md' $h0)
}
$diskMap = @{
    'skills/replace.md'    = (New-DiskEntry 'skills/replace.md' $h0)
    'skills/same.md'       = (New-DiskEntry 'skills/same.md' $h2)
    'skills/modified.md'   = (New-DiskEntry 'skills/modified.md' $h9)
    'skills/collide.md'    = (New-DiskEntry 'skills/collide.md' $h9)
    'skills/retire.md'     = (New-DiskEntry 'skills/retire.md' $h0)
    'skills/mod-retired.md' = (New-DiskEntry 'skills/mod-retired.md' $h9)
    'skills/stray.md'      = (New-DiskEntry 'skills/stray.md' $h7)
    'skills/dir-where-file.md' = (New-DiskEntry 'skills/dir-where-file.md' $null -IsDir)
    'skills/link.md'       = (New-DiskEntry 'skills/link.md' $null -Reparse)
}

$plan = Build-FilePlan -NewMap $newMap -OldMap $oldMap -DiskMap $diskMap
$planByPath = @{}
foreach ($e in $plan) { $planByPath[$e.path] = $e }

function Assert-Action {
    param([string]$Name, [string]$Path, [string]$ExpectedAction, [bool]$ExpectedBackup)
    $e = $planByPath[$Path]
    if ($null -eq $e) {
        Assert-Equal -Name "$Name(无计划项)" -Expected $ExpectedAction -Actual '<absent>'
        return
    }
    Assert-Equal -Name $Name -Expected $ExpectedAction -Actual $e.action
    Assert-Equal -Name "$Name 备份标记" -Expected $ExpectedBackup -Actual ([bool]$e.backup)
}

Assert-Action '官方未改→换新' 'skills/replace.md' 'replace' $true
Assert-Action '内容已是新版→不动' 'skills/same.md' 'skip-current' $false
Assert-Action '新版新增→装' 'skills/new.md' 'install-new' $false
Assert-Action '本地改过官方件→冲突保留' 'skills/modified.md' 'conflict-modified' $true
Assert-Action '未知文件撞新版同路径→冲突保留' 'skills/collide.md' 'conflict-collision' $true
Assert-Action '官方件本地删掉→不复活' 'skills/missing.md' 'missing-kept' $false
Assert-Action '官方删且本地未改→退役' 'skills/retire.md' 'retire' $true
Assert-Action '本地改过且新版已删→保留' 'skills/mod-retired.md' 'keep-modified-retired' $false
Assert-Action '未知文件→保留' 'skills/stray.md' 'keep-unknown' $false
Assert-Action '目录挡文件路径→不写不删' 'skills/dir-where-file.md' 'conflict-kind' $false
Assert-Action '链接挡文件路径→不写不删' 'skills/link.md' 'conflict-reparse' $false
Assert-Equal -Name '旧清单有但盘面无痕且新版无→不进计划' `
    -Expected $false -Actual $planByPath.ContainsKey('skills/ghost.md')
Assert-Equal -Name '计划项总数正好' -Expected 11 -Actual $plan.Count

# =====================================================================
# 集成:v1 安装(带记录) → 本地改动 → v2 升级,逐类断言去向。
# =====================================================================
Write-Host ""
Write-Host "==== 所有权落地(v1 记档 → v2 升级)====" -ForegroundColor Cyan

$ioRoot = Join-Path ([IO.Path]::GetTempPath()) ("lubancode-own-test-" + [Guid]::NewGuid().ToString('N'))
try {
    $v1pkg = Join-Path $ioRoot 'v1pkg'
    $v2pkg = Join-Path $ioRoot 'v2pkg'
    $inst = Join-Path $ioRoot 'install'
    $userHome = Join-Path $ioRoot 'userhome'

    # --- v1 包 ---
    New-Item -ItemType Directory -Path (Join-Path $v1pkg 'skills\lubancode-config\references') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $v1pkg 'skills\extra') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $v1pkg 'skills\hack') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $v1pkg 'docs') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $v1pkg 'web\assistant') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $v1pkg 'licenses') -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $v1pkg 'lubancode.exe') -Value 'fake exe v1' -Encoding UTF8
    Set-Content -LiteralPath (Join-Path $v1pkg 'skills\lubancode-config\SKILL.md') -Value 'v1 skill' -Encoding UTF8
    Set-Content -LiteralPath (Join-Path $v1pkg 'skills\lubancode-config\references\map.md') -Value 'v1 map' -Encoding UTF8
    Set-Content -LiteralPath (Join-Path $v1pkg 'skills\extra\old-official.md') -Value 'retire me' -Encoding UTF8
    Set-Content -LiteralPath (Join-Path $v1pkg 'skills\hack\keep.md') -Value 'v1 hacked-origin' -Encoding UTF8
    Set-Content -LiteralPath (Join-Path $v1pkg 'docs\README.md') -Value 'v1 docs' -Encoding UTF8
    Set-Content -LiteralPath (Join-Path $v1pkg 'web\assistant\index.html') -Value 'v1 web' -Encoding UTF8
    Set-Content -LiteralPath (Join-Path $v1pkg 'LICENSE') -Value 'MIT same both' -Encoding UTF8
    Set-Content -LiteralPath (Join-Path $v1pkg 'licenses\ripgrep-MIT.txt') -Value 'v1 license' -Encoding UTF8
    $m1 = New-ManifestFromTree -RootDir $v1pkg

    # --- 安装目录:v1 官方件落地 + 本地改动 + 未知件 + 删一件 ---
    New-Item -ItemType Directory -Path (Join-Path $inst 'skills\lubancode-config\references') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $inst 'skills\extra') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $inst 'skills\hack') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $inst 'web\assistant') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $inst 'licenses') -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $inst 'lubancode.exe') -Value 'fake exe v1' -Encoding UTF8
    Set-Content -LiteralPath (Join-Path $inst 'skills\lubancode-config\SKILL.md') -Value 'v1 skill' -Encoding UTF8
    Set-Content -LiteralPath (Join-Path $inst 'skills\lubancode-config\references\map.md') -Value 'my local map' -Encoding UTF8
    Set-Content -LiteralPath (Join-Path $inst 'skills\extra\old-official.md') -Value 'retire me' -Encoding UTF8
    Set-Content -LiteralPath (Join-Path $inst 'skills\hack\keep.md') -Value 'locally hacked' -Encoding UTF8
    Set-Content -LiteralPath (Join-Path $inst 'web\assistant\index.html') -Value 'v1 web' -Encoding UTF8
    Set-Content -LiteralPath (Join-Path $inst 'web\assistant\app.js') -Value 'unknown app' -Encoding UTF8
    Set-Content -LiteralPath (Join-Path $inst 'web\assistant\custom.css') -Value 'user css' -Encoding UTF8
    Set-Content -LiteralPath (Join-Path $inst 'LICENSE') -Value 'MIT same both' -Encoding UTF8
    # docs/README.md 与 licenses/ripgrep-MIT.txt 有意不落地:本地删掉官方件
    # 假用户根:安装全程不得触碰
    New-Item -ItemType Directory -Path (Join-Path $userHome 'skills\mine') -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $userHome 'skills\mine\SKILL.md') -Value 'user skill untouched' -Encoding UTF8

    # 安装记录:嵌 v1 官方清单
    $state = [ordered]@{
        schema = 1
        version = '0.1.0'
        install_mode = 'test-fixture'
        manifest = $m1
        pending_conflicts = @()
    }
    $stateJson = ConvertTo-Json -InputObject $state -Depth 8
    Set-Content -LiteralPath (Join-Path $inst 'install-state.json') -Value $stateJson -Encoding UTF8

    # --- v2 包 ---
    New-Item -ItemType Directory -Path (Join-Path $v2pkg 'skills\lubancode-config\references') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $v2pkg 'skills\hack') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $v2pkg 'docs') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $v2pkg 'web\assistant') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $v2pkg 'licenses') -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $v2pkg 'lubancode.exe') -Value 'fake exe v2' -Encoding UTF8
    Set-Content -LiteralPath (Join-Path $v2pkg 'skills\lubancode-config\SKILL.md') -Value 'v2 skill' -Encoding UTF8
    Set-Content -LiteralPath (Join-Path $v2pkg 'skills\lubancode-config\references\map.md') -Value 'v2 map' -Encoding UTF8
    Set-Content -LiteralPath (Join-Path $v2pkg 'skills\hack\brand-new.md') -Value 'v2 addition' -Encoding UTF8
    Set-Content -LiteralPath (Join-Path $v2pkg 'docs\README.md') -Value 'v2 docs' -Encoding UTF8
    Set-Content -LiteralPath (Join-Path $v2pkg 'web\assistant\index.html') -Value 'v2 web' -Encoding UTF8
    Set-Content -LiteralPath (Join-Path $v2pkg 'web\assistant\app.js') -Value 'v2 app' -Encoding UTF8
    Set-Content -LiteralPath (Join-Path $v2pkg 'LICENSE') -Value 'MIT same both' -Encoding UTF8
    Set-Content -LiteralPath (Join-Path $v2pkg 'licenses\ripgrep-MIT.txt') -Value 'v2 license' -Encoding UTF8
    $m2 = New-ManifestFromTree -RootDir $v2pkg

    $treeMap = Get-TreeMap -InstallDir $inst
    $disk = Get-InstallDiskState -InstallRoot $inst -TreeMap $treeMap
    $plan2 = Build-FilePlan -NewMap (Get-ManifestFileMap -Manifest $m2) `
        -OldMap (Get-ManifestFileMap -Manifest $m1) -DiskMap $disk
    $result = Invoke-ResourceApply -Plan $plan2 -NewManifest $m2 -SourceRoot $v2pkg -InstallRoot $inst `
        -TreeMap $treeMap -SourceMeta @{ repo = 't/t' } -InstallMode 'test'

    # 官方未改 → 换新
    Assert-FileText -Name '官方未改的 SKILL.md 换到 v2' -Path (Join-Path $inst 'skills\lubancode-config\SKILL.md') -ExpectedText 'v2 skill'
    Assert-FileText -Name '官方未改的 index.html 换到 v2' -Path (Join-Path $inst 'web\assistant\index.html') -ExpectedText 'v2 web'
    Assert-FileText -Name 'exe 换到 v2' -Path (Join-Path $inst 'lubancode.exe') -ExpectedText 'fake exe v2'
    # 内容已是新版 → 不动
    Assert-FileText -Name 'LICENSE 内容未变(skip-current)' -Path (Join-Path $inst 'LICENSE') -ExpectedText 'MIT same both'
    # 新版新增 → 装
    Assert-FileText -Name '新版新增 brand-new.md 已装' -Path (Join-Path $inst 'skills\hack\brand-new.md') -ExpectedText 'v2 addition'
    # 本地改过 → 原地保留,新版不落
    Assert-FileText -Name '本地改过的 map.md 原样保留' -Path (Join-Path $inst 'skills\lubancode-config\references\map.md') -ExpectedText 'my local map'
    # 未知文件撞新版同路径 → 保留,新版不落
    Assert-FileText -Name '未知 app.js 保留(不装 v2)' -Path (Join-Path $inst 'web\assistant\app.js') -ExpectedText 'unknown app'
    # 未知文件(新版也无)→ 保留
    Assert-FileText -Name '未知 custom.css 保留' -Path (Join-Path $inst 'web\assistant\custom.css') -ExpectedText 'user css'
    # 官方删了且本地未改 → 退役
    Assert-Equal -Name '新版删掉的 old-official.md 已退役' -Expected $false `
        -Actual (Test-Path -LiteralPath (Join-Path $inst 'skills\extra\old-official.md'))
    Assert-Equal -Name '退役后的空目录 skills\extra 已收尾' -Expected $false `
        -Actual (Test-Path -LiteralPath (Join-Path $inst 'skills\extra'))
    # 本地改过且新版删了 → 保留
    Assert-FileText -Name '本地改过的 keep.md 保留' -Path (Join-Path $inst 'skills\hack\keep.md') -ExpectedText 'locally hacked'
    # 官方件本地删掉 → 不复活
    Assert-Equal -Name '本地删掉的 docs/README.md 不复活' -Expected $false `
        -Actual (Test-Path -LiteralPath (Join-Path $inst 'docs\README.md'))
    # 备份:换新/退役/冲突的原件都在
    $backupRoot = $result.backupRoot
    Assert-FileText -Name '备份里有 v1 SKILL.md' -Path (Join-Path $backupRoot 'skills\lubancode-config\SKILL.md') -ExpectedText 'v1 skill'
    Assert-FileText -Name '备份里有被退役的 old-official.md' -Path (Join-Path $backupRoot 'skills\extra\old-official.md') -ExpectedText 'retire me'
    Assert-FileText -Name '备份里有本地改过的 map.md' -Path (Join-Path $backupRoot 'skills\lubancode-config\references\map.md') -ExpectedText 'my local map'
    Assert-FileText -Name '备份里有未知 app.js' -Path (Join-Path $backupRoot 'web\assistant\app.js') -ExpectedText 'unknown app'
    Assert-FileText -Name '备份里有 v1 web' -Path (Join-Path $backupRoot 'web\assistant\index.html') -ExpectedText 'v1 web'
    # 记录件:新清单入档,冲突入账
    $newState = Get-Content -LiteralPath (Join-Path $inst 'install-state.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    Assert-Equal -Name '新 install-state 记了 v2 清单' `
        -Expected @($m2.files).Count -Actual @($newState.manifest.files).Count
    $conflictPaths = @($newState.pending_conflicts | ForEach-Object { $_.path })
    Assert-True -Name '冲突账里有 map.md' -Actual ($conflictPaths -contains 'skills/lubancode-config/references/map.md')
    Assert-True -Name '冲突账里有 app.js' -Actual ($conflictPaths -contains 'web/assistant/app.js')
    Assert-True -Name '冲突账里有 keep.md(改过且新版删)' -Actual ($conflictPaths -contains 'skills/hack/keep.md')
    Assert-True -Name 'manifest.json 已落安装根' -Actual (Test-Path -LiteralPath (Join-Path $inst 'manifest.json') -PathType Leaf)
    # 用户根:全程不动
    Assert-FileText -Name '用户根技能原封不动' -Path (Join-Path $userHome 'skills\mine\SKILL.md') -ExpectedText 'user skill untouched'

    # --- Read-InstallBaseline:install-state 优先于根下平铺 manifest.json ---
    $readBack = Read-InstallBaseline -InstallRoot $inst
    Assert-Equal -Name '基线读取认 install-state 的清单' `
        -Expected @($m2.files).Count -Actual @($readBack.files).Count

    # --- 重复安装(v2 再装一遍):官方件零改动;本地差异持续点名 ---
    $treeMap = Get-TreeMap -InstallDir $inst
    $disk2 = Get-InstallDiskState -InstallRoot $inst -TreeMap $treeMap
    $plan3 = Build-FilePlan -NewMap (Get-ManifestFileMap -Manifest $m2) `
        -OldMap (Get-ManifestFileMap -Manifest (Read-InstallBaseline -InstallRoot $inst)) -DiskMap $disk2
    $actionCounts = @{}
    foreach ($e in $plan3) {
        if (-not $actionCounts.ContainsKey($e.action)) { $actionCounts[$e.action] = 0 }
        $actionCounts[$e.action]++
    }
    Write-Host ("重复安装动作分布:" + (($actionCounts.Keys | Sort-Object | ForEach-Object { "$($_)=$($actionCounts[$_])" }) -join ', '))
    foreach ($e in $plan3) { Write-Host "  [repeat] $($e.action)  $($e.path)" }
    function Get-CountOf {
        param([hashtable]$Counts, [string]$Key)
        if ($Counts.ContainsKey($Key)) { return $Counts[$Key] }
        return 0
    }
    Assert-Equal -Name '重复安装:官方未动件全 skip-current' -Expected 5 -Actual (Get-CountOf $actionCounts 'skip-current')
    Assert-Equal -Name '重复安装:本地改动件持续报冲突(上轮同路径冲突件转为本地修改记账)' -Expected 2 -Actual (Get-CountOf $actionCounts 'conflict-modified')
    Assert-Equal -Name '重复安装:本地删掉的官方件持续不复活' -Expected 2 -Actual (Get-CountOf $actionCounts 'missing-kept')
    Assert-Equal -Name '重复安装:未知文件持续保留' -Expected 2 -Actual (Get-CountOf $actionCounts 'keep-unknown')
    Assert-Equal -Name '重复安装:普通容器目录不进计划(conflict-kind 为零)' -Expected 0 -Actual (Get-CountOf $actionCounts 'conflict-kind')
    Assert-True -Name '重复安装:零改动项(replace/install-new/retire)' `
        -Actual (-not ($actionCounts.ContainsKey('replace') -or $actionCounts.ContainsKey('install-new') -or $actionCounts.ContainsKey('retire')))

    # --- 预演(Scan)不动盘面:Invoke-Install 全流程走一遍 ---
    $before = Get-ChildItem -LiteralPath $inst -Recurse -File | Sort-Object -Property FullName
    $beforeHash = @{}
    foreach ($f in $before) { $beforeHash[$f.FullName] = (Get-FileSha256 -Path $f.FullName) }
    Invoke-Install -InstallDir $inst -SourceExe (Join-Path $v2pkg 'lubancode.exe') -Scan -SkipPath | Out-Null
    $after = Get-ChildItem -LiteralPath $inst -Recurse -File | Sort-Object -Property FullName
    $allSame = $true
    if ($before.Count -ne @($after).Count) { $allSame = $false }
    foreach ($f in $after) {
        if (-not $beforeHash.ContainsKey($f.FullName)) { $allSame = $false; break }
        if ($beforeHash[$f.FullName] -ne (Get-FileSha256 -Path $f.FullName)) { $allSame = $false; break }
    }
    Assert-True -Name 'Scan 预演不动盘面一个字节' -Actual $allSame
} finally {
    if (Test-Path -LiteralPath $ioRoot) {
        Remove-Item -LiteralPath $ioRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}

# =====================================================================
# 无基线旧安装:按新包白名单覆盖;旧参数也不能删除包外文件。
# =====================================================================
Write-Host ""
Write-Host "==== 无基线白名单覆盖 / 旧参数兼容 ====" -ForegroundColor Cyan

$nrRoot = Join-Path ([IO.Path]::GetTempPath()) ("lubancode-nr-test-" + [Guid]::NewGuid().ToString('N'))
try {
    $pkg = Join-Path $nrRoot 'pkg'
    $inst = Join-Path $nrRoot 'install'
    New-Item -ItemType Directory -Path (Join-Path $pkg 'skills\mine-skill') -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $pkg 'lubancode.exe') -Value 'fake exe new' -Encoding UTF8
    Set-Content -LiteralPath (Join-Path $pkg 'skills\mine-skill\SKILL.md') -Value 'new official' -Encoding UTF8
    # 旧安装:无任何记录件,有一枚用户技能与 exe
    New-Item -ItemType Directory -Path (Join-Path $inst 'skills\mine-skill') -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $inst 'lubancode.exe') -Value 'fake exe old' -Encoding UTF8
    Set-Content -LiteralPath (Join-Path $inst 'skills\mine-skill\SKILL.md') -Value 'precious user skill' -Encoding UTF8

    # 子进程跑默认入口:无需基线,备份后逐文件覆盖。
    # Windows 有 powershell(5.1),ubuntu 只有 pwsh——按环境挑。
    $childPs = 'powershell'
    if (-not (Get-Command powershell -ErrorAction SilentlyContinue)) { $childPs = 'pwsh' }
    $out = & $childPs -NoProfile -ExecutionPolicy Bypass -File $installScript `
        -SourceExe (Join-Path $pkg 'lubancode.exe') -InstallDir $inst -SkipPath 2>&1
    Assert-True -Name '无基线安装不再因缺清单退 3(假 exe 最后校验可退 1)' -Actual ($LASTEXITCODE -in @(0, 1))
    Assert-FileText -Name '白名单同名技能已换新' `
        -Path (Join-Path $inst 'skills\mine-skill\SKILL.md') -ExpectedText 'new official'
    Assert-FileText -Name '无基线也能换新 exe' -Path (Join-Path $inst 'lubancode.exe') -ExpectedText 'fake exe new'
    $backupDirs = @(Get-ChildItem -LiteralPath (Join-Path $inst 'backups') -Directory -ErrorAction SilentlyContinue)
    Assert-Equal -Name '白名单覆盖前保存旧文件' -Expected 1 -Actual $backupDirs.Count
    Assert-FileText -Name '完整备份里有用户技能原件' `
        -Path (Join-Path $backupDirs[0].FullName 'skills\mine-skill\SKILL.md') -ExpectedText 'precious user skill'

    # 旧参数兼容:仍按白名单逐项更新
    $out2 = & $childPs -NoProfile -ExecutionPolicy Bypass -File $installScript `
        -SourceExe (Join-Path $pkg 'lubancode.exe') -InstallDir $inst -SkipPath -AllowUnknownReplace 2>&1
    # 假 exe 跑不动 --version:Windows 下整脚本会在最后一步校验失败退 1;
    # Linux 的 pwsh 对不可执行文件兜到 xdg-open,退出码不定,替换结果为准
    if ($env:OS -eq 'Windows_NT') {
        Assert-Equal -Name '整目录确认后假 exe 版本校验退 1(替换已完成)' -Expected 1 -Actual $LASTEXITCODE
    } else {
        Assert-True -Name '整目录确认后假 exe 兜底退出码可接受(Linux 以替换结果为准)' `
            -Actual (@(0, 1) -contains $LASTEXITCODE)
    }
    Assert-FileText -Name '整目录替换后新版官方技能落地' `
        -Path (Join-Path $inst 'skills\mine-skill\SKILL.md') -ExpectedText 'new official'
    Assert-True -Name '替换后用户原件仍在备份里' -Actual (Test-Path -LiteralPath (Join-Path $backupDirs[0].FullName 'skills\mine-skill\SKILL.md'))
    Assert-True -Name '替换后又多了一份备份' `
        -Actual (@(Get-ChildItem -LiteralPath (Join-Path $inst 'backups') -Directory).Count -ge 2)
    Assert-True -Name '整目录确认后写了安装记录' `
        -Actual (Test-Path -LiteralPath (Join-Path $inst 'install-state.json') -PathType Leaf)

    # BackupOnly:再补一份完整备份,不安装
    $out3 = & $childPs -NoProfile -ExecutionPolicy Bypass -File $installScript -InstallDir $inst -BackupOnly 2>&1
    Assert-Equal -Name 'BackupOnly 正常退出' -Expected 0 -Actual $LASTEXITCODE
    Assert-True -Name 'BackupOnly 又添一份备份' `
        -Actual (@(Get-ChildItem -LiteralPath (Join-Path $inst 'backups') -Directory).Count -ge 3)
    Assert-FileText -Name 'BackupOnly 不动盘面' -Path (Join-Path $inst 'skills\mine-skill\SKILL.md') -ExpectedText 'new official'
} finally {
    if (Test-Path -LiteralPath $nrRoot) {
        Remove-Item -LiteralPath $nrRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}

# =====================================================================
# 同目录安装:来源=目标,不删目录再搬自己,只补记档。
# =====================================================================
Write-Host ""
Write-Host "==== 同目录安装 ====" -ForegroundColor Cyan

$sdRoot = Join-Path ([IO.Path]::GetTempPath()) ("lubancode-samedir-test-" + [Guid]::NewGuid().ToString('N'))
try {
    $pkg = Join-Path $sdRoot 'pkg'
    New-Item -ItemType Directory -Path (Join-Path $pkg 'skills\lubancode-config') -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $pkg 'lubancode.exe') -Value 'fake exe' -Encoding UTF8
    Set-Content -LiteralPath (Join-Path $pkg 'skills\lubancode-config\SKILL.md') -Value 'in-place skill' -Encoding UTF8
    Set-Content -LiteralPath (Join-Path $pkg 'user-note.txt') -Value 'user keeps this' -Encoding UTF8

    $before = Get-ChildItem -LiteralPath $pkg -Recurse -File | Sort-Object -Property FullName
    $beforeHash = @{}
    foreach ($f in $before) { $beforeHash[$f.FullName] = (Get-FileSha256 -Path $f.FullName) }

    Invoke-Install -InstallDir $pkg -SourceExe (Join-Path $pkg 'lubancode.exe') -SkipPath | Out-Null

    $after = Get-ChildItem -LiteralPath $pkg -Recurse -File | Sort-Object -Property FullName
    # 允许多出来的只有记录件(manifest.json/install-state.json);原有文件一个不能少、一字不能动
    $allSame = $true
    foreach ($f in $after) {
        if ($f.Name -in @('manifest.json', 'install-state.json')) { continue }
        if (-not $beforeHash.ContainsKey($f.FullName)) { $allSame = $false; break }
        if ($beforeHash[$f.FullName] -ne (Get-FileSha256 -Path $f.FullName)) { $allSame = $false; break }
    }
    Assert-True -Name '同目录安装不删不搬任何现有文件' -Actual $allSame
    Assert-FileText -Name '同目录安装后用户旁文件仍在' -Path (Join-Path $pkg 'user-note.txt') -ExpectedText 'user keeps this'
    Assert-True -Name '同目录安装补了记档' -Actual (Test-Path -LiteralPath (Join-Path $pkg 'install-state.json') -PathType Leaf)
} finally {
    if (Test-Path -LiteralPath $sdRoot) {
        Remove-Item -LiteralPath $sdRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Write-Host ""
. (Join-Path $scriptDir 'install.legacy.tests.ps1')

Write-Host "共 $($script:passCount + $script:failCount) 项,通过 $($script:passCount),失败 $($script:failCount)" -ForegroundColor $(if ($script:failCount -eq 0) { 'Green' } else { 'Red' })

if ($script:failCount -gt 0) {
    exit 1
}
