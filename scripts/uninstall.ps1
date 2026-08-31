#Requires -Version 5.1
<#
    lubancode 卸载脚本(Windows,PowerShell 5.1 兼容)

    用法:
        .\uninstall.ps1                          卸载默认目录(%LOCALAPPDATA%\Programs\lubancode)
        .\uninstall.ps1 -InstallDir D:\tools\lb   卸载指定目录(要跟当初 install.ps1 -InstallDir 传的一致)
        .\uninstall.ps1 -SkipPath                 不动用户 PATH(测试用)

    不需要管理员权限。目录本来就不存在时,视为"本来就没装",不当错误处理,
    但仍会顺手检查一下 PATH 里有没有残留条目并清掉(除非 -SkipPath)。
    只删 LubanCode 安装目录内的东西——含随包 ripgrep(安装目录 libexec\
    rg.exe)与 licenses/、THIRD_PARTY_NOTICES.md;用户装在别处的 rg(比如
    scoop/winget 装的)一概不碰。
#>
[CmdletBinding()]
param(
    [string]$InstallDir,
    [switch]$SkipPath
)

$ErrorActionPreference = 'Stop'

$AppName = 'lubancode'

# ===================== 输出小工具 =====================

function Write-Step {
    param([string]$Msg)
    Write-Host "==> $Msg" -ForegroundColor Cyan
}

function Write-ErrStep {
    param([string]$Msg)
    Write-Host "错误:$Msg" -ForegroundColor Red
}

# ===================== 纯逻辑函数(跟 install.ps1 保持一致,方便单独分发/单测) =====================

function Get-DefaultInstallDir {
    return Join-Path $env:LOCALAPPDATA "Programs\$AppName"
}

function Get-PathEntries {
    param([string]$PathValue)
    if ([string]::IsNullOrEmpty($PathValue)) { return @() }
    return @($PathValue -split ';' | Where-Object { $_ -ne '' })
}

function Get-UpdatedPathForRemove {
    <#
        纯函数:给旧 PATH 字符串和待摘除目录,算出新的 PATH 字符串。
        目录不在其中时原样返回旧值,调用方可用字符串相等判断"未改动"。
    #>
    param(
        [string]$OldValue,
        [string]$DirToRemove
    )
    $normalizedTarget = $DirToRemove.TrimEnd('\').ToLowerInvariant()
    $entries = Get-PathEntries -PathValue $OldValue
    $kept = @($entries | Where-Object { $_.TrimEnd('\').ToLowerInvariant() -ne $normalizedTarget })
    return ($kept -join ';')
}

# ===================== 注册表读写(跟 install.ps1 同样的坑:REG_EXPAND_SZ 别展开写死) =====================

function Get-UserPathRaw {
    $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Environment')
    if (-not $key) { return '' }
    try {
        $val = $key.GetValue('Path', '', [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
        if ($null -eq $val) { return '' }
        return [string]$val
    } finally {
        $key.Close()
    }
}

function Set-UserPathRaw {
    param([string]$NewValue)
    $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Environment', $true)
    if (-not $key) {
        throw "打不开 HKCU:\Environment 写权限"
    }
    try {
        $key.SetValue('Path', $NewValue, [Microsoft.Win32.RegistryValueKind]::ExpandString)
    } finally {
        $key.Close()
    }
}

function Send-EnvironmentChangeBroadcast {
    if (-not ([System.Management.Automation.PSTypeName]'LubancodeInstaller.NativeMethods').Type) {
        Add-Type -Namespace LubancodeInstaller -Name NativeMethods -MemberDefinition @'
[DllImport("user32.dll", SetLastError = true, CharSet = CharSet.Auto)]
public static extern IntPtr SendMessageTimeout(IntPtr hWnd, uint Msg, UIntPtr wParam, string lParam, uint fuFlags, uint uTimeout, out UIntPtr lpdwResult);
'@
    }
    $HWND_BROADCAST = [IntPtr]0xffff
    $WM_SETTINGCHANGE = 0x1A
    $SMTO_ABORTIFHUNG = 0x2
    $result = [UIntPtr]::Zero
    [LubancodeInstaller.NativeMethods]::SendMessageTimeout($HWND_BROADCAST, $WM_SETTINGCHANGE, [UIntPtr]::Zero, 'Environment', $SMTO_ABORTIFHUNG, 5000, [ref]$result) | Out-Null
}

function Remove-DirFromUserPath {
    param([string]$Dir)
    $old = Get-UserPathRaw
    $new = Get-UpdatedPathForRemove -OldValue $old -DirToRemove $Dir
    if ($new -eq $old) {
        Write-Step "用户 PATH 里没有 $Dir,跳过。"
        return
    }
    Set-UserPathRaw -NewValue $new
    Send-EnvironmentChangeBroadcast
    Write-Step "已从用户 PATH 摘除 $Dir。"
}

# ===================== 主流程 =====================

function Invoke-Uninstall {
    param(
        [string]$InstallDir,
        [switch]$SkipPath
    )

    if (-not $InstallDir) { $InstallDir = Get-DefaultInstallDir }

    if (-not (Test-Path -LiteralPath $InstallDir)) {
        Write-Host "本来就没装(找不到 $InstallDir)。" -ForegroundColor Yellow
    } else {
        try {
            Remove-Item -LiteralPath $InstallDir -Recurse -Force
        } catch {
            Write-ErrStep "删除安装目录 $InstallDir 失败(是不是 lubancode 正在运行?先关掉再重试):$($_.Exception.Message)"
            exit 1
        }
        Write-Step "已删除 $InstallDir"
    }

    if ($SkipPath) {
        Write-Step "已跳过 PATH 清理(-SkipPath)。"
    } else {
        try {
            Remove-DirFromUserPath -Dir $InstallDir
        } catch {
            Write-ErrStep "从用户 PATH 摘除失败:$($_.Exception.Message)"
            exit 1
        }
    }

    Write-Host "卸载完成。重开一个新的终端窗口,PATH 变化才会生效。" -ForegroundColor Green
}

if ($MyInvocation.InvocationName -ne '.') {
    Invoke-Uninstall -InstallDir $InstallDir -SkipPath:$SkipPath
}
