#Requires -Version 5.1
<#
    install.ps1 / uninstall.ps1 里 PATH 纯逻辑函数的手工验证脚本。

    不进 ctest,手工跑:
        powershell -NoProfile -File scripts\tests\install.tests.ps1

    只测"计算新 PATH 字符串"这部分纯函数(Get-UpdatedPathForAdd / Get-UpdatedPathForRemove),
    不碰真实 HKCU 注册表——通过点号(.)dot-source install.ps1,脚本内部靠
    "$MyInvocation.InvocationName -ne '.'" 这个判断,dot-source 时不会触发真正的安装动作。
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
    param(
        [string]$Name,
        [bool]$Actual
    )
    Assert-Equal -Name $Name -Expected $true -Actual $Actual
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

Assert-Equal -Name '默认安装目录拼接正确' `
    -Expected (Join-Path $env:LOCALAPPDATA 'Programs\lubancode') `
    -Actual (Get-DefaultInstallDir)

Write-Host ""
Write-Host "==== Sync-OfficialSkills ====" -ForegroundColor Cyan

$syncRoot = Join-Path $env:TEMP ("lubancode-skills-test-" + [Guid]::NewGuid().ToString('N'))
try {
    $sourceDir = Join-Path $syncRoot 'source'
    $sourceSkills = Join-Path $sourceDir 'skills\lubancode-config\references'
    $installDir = Join-Path $syncRoot 'install'
    $oldSkills = Join-Path $installDir 'skills\lubancode-config'
    New-Item -ItemType Directory -Path $sourceSkills -Force | Out-Null
    New-Item -ItemType Directory -Path $oldSkills -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $sourceDir 'lubancode.exe') -Value 'fake exe' -Encoding UTF8
    Set-Content -LiteralPath (Join-Path $sourceDir 'skills\lubancode-config\SKILL.md') -Value 'new router' -Encoding UTF8
    Set-Content -LiteralPath (Join-Path $sourceSkills 'soul-and-prompts.md') -Value 'new reference' -Encoding UTF8
    Set-Content -LiteralPath (Join-Path $oldSkills 'old.md') -Value 'old reference' -Encoding UTF8

    Sync-OfficialSkills -SourceExe (Join-Path $sourceDir 'lubancode.exe') -InstallDir $installDir

    Assert-True -Name '同步后入口技能存在' `
        -Actual (Test-Path -LiteralPath (Join-Path $installDir 'skills\lubancode-config\SKILL.md') -PathType Leaf)
    Assert-True -Name '同步后 reference 跟着过去' `
        -Actual (Test-Path -LiteralPath (Join-Path $installDir 'skills\lubancode-config\references\soul-and-prompts.md') -PathType Leaf)
    Assert-Equal -Name '旧官方技能文件已清掉' `
        -Expected $false `
        -Actual (Test-Path -LiteralPath (Join-Path $installDir 'skills\lubancode-config\old.md'))
} finally {
    if (Test-Path -LiteralPath $syncRoot) {
        Remove-Item -LiteralPath $syncRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Write-Host ""
Write-Host "共 $($script:passCount + $script:failCount) 项,通过 $($script:passCount),失败 $($script:failCount)" -ForegroundColor $(if ($script:failCount -eq 0) { 'Green' } else { 'Red' })

if ($script:failCount -gt 0) {
    exit 1
}
