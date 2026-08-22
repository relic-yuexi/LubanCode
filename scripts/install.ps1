#Requires -Version 5.1
<#
    lubancode 安装脚本(Windows,PowerShell 5.1 兼容)

    用法:
        .\install.ps1                          自动模式:脚本同目录找得到 lubancode.exe 就本地装,
                                                 找不到就去 GitHub 最新 release 下载
        .\install.ps1 -SourceExe C:\a\b.exe     指定本地可执行文件路径(跳过自动查找/下载)
        .\install.ps1 -InstallDir D:\tools\lb   自定义安装目录(默认 %LOCALAPPDATA%\Programs\lubancode)
        .\install.ps1 -SkipPath                 不写用户 PATH(测试/CI 用,避免污染真实环境)

    不需要管理员权限——只动当前用户的安装目录和 HKCU 用户级 PATH,不碰系统级 PATH。
    重复跑等价于覆盖升级(幂等)。
#>
[CmdletBinding()]
param(
    [string]$InstallDir,
    [string]$SourceExe,
    [switch]$SkipPath
)

$ErrorActionPreference = 'Stop'

$Repo = 'relic-yuexi/LubanCode'

$AppName = 'lubancode'
$ExeName = 'lubancode.exe'

# ===================== 输出小工具 =====================

function Write-Step {
    param([string]$Msg)
    Write-Host "==> $Msg" -ForegroundColor Cyan
}

function Write-ErrStep {
    param([string]$Msg)
    Write-Host "错误:$Msg" -ForegroundColor Red
}

# ===================== 纯逻辑函数(不碰注册表/文件系统,方便单测) =====================

function Get-DefaultInstallDir {
    # %LOCALAPPDATA% 在正常 Windows 用户会话下必定存在,不做额外兜底
    return Join-Path $env:LOCALAPPDATA "Programs\$AppName"
}

function Get-PathEntries {
    <# 把一段 PATH 字符串拆成条目数组,过滤掉空段(常见于结尾多余的分号) #>
    param([string]$PathValue)
    if ([string]::IsNullOrEmpty($PathValue)) { return @() }
    return @($PathValue -split ';' | Where-Object { $_ -ne '' })
}

function Test-PathContainsDir {
    param([string]$PathValue, [string]$Dir)
    $normalizedTarget = $Dir.TrimEnd('\').ToLowerInvariant()
    foreach ($entry in (Get-PathEntries -PathValue $PathValue)) {
        if ($entry.TrimEnd('\').ToLowerInvariant() -eq $normalizedTarget) {
            return $true
        }
    }
    return $false
}

function Get-UpdatedPathForAdd {
    <#
        纯函数:给旧 PATH 字符串和待加目录,算出新的 PATH 字符串。
        已经包含该目录(大小写不敏感、忽略结尾反斜杠差异)时返回 $null,表示不需要改。
        不读写注册表,方便单测覆盖各种拼接/去重场景。
    #>
    param(
        [string]$OldValue,
        [string]$NewDir
    )
    if (Test-PathContainsDir -PathValue $OldValue -Dir $NewDir) {
        return $null
    }
    $trimmedOld = $OldValue
    if ($trimmedOld) { $trimmedOld = $trimmedOld.TrimEnd(';') }
    if ([string]::IsNullOrEmpty($trimmedOld)) {
        return $NewDir
    }
    return "$trimmedOld;$NewDir"
}

function Get-UpdatedPathForRemove {
    <#
        纯函数:给旧 PATH 字符串和待摘除目录,算出新的 PATH 字符串。
        目录不在其中时,原样返回旧值(调用方可用字符串相等判断"未改动")。
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

function Add-DirToProcessPath {
    <#
        把目录补进当前 PowerShell 进程的 PATH。
        写 HKCU 只管往后新生的进程；Windows Terminal 常从旧父进程开新标签，
        仍会继承旧 PATH。安装脚本直接运行时补这一层，命令当场便能找到。
    #>
    param([string]$Dir)
    $new = Get-UpdatedPathForAdd -OldValue $env:Path -NewDir $Dir
    if ($null -eq $new) {
        return $false
    }
    $env:Path = $new
    return $true
}

# ===================== 注册表读写(REG_EXPAND_SZ 语义要小心) =====================
#
# 坑在这儿:HKCU\Environment\Path 的类型是 REG_EXPAND_SZ,里面可能含有
# %SystemRoot% 这类未展开的变量引用。如果用会自动展开的接口去读(比如
# PowerShell 的 [Environment]::GetEnvironmentVariable 不传 Target 时,或者某些
# Get-ItemProperty 路径),再原样写回去,就会把 %VAR% 永久展开成具体路径焗死,
# 而且每次追加都可能把值越写越长、越写越"死"。所以这里统一用
# Microsoft.Win32.Registry 的 GetValue(..., DoNotExpandEnvironmentNames) 读原始
# 字面值,写回时显式声明 RegistryValueKind.ExpandString,保证 %VAR% 原样保留。

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
    <# 广播 WM_SETTINGCHANGE,让新开的资源管理器/终端能立刻感知到 PATH 变化 #>
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

function Add-DirToUserPath {
    param([string]$Dir)
    $old = Get-UserPathRaw
    $new = Get-UpdatedPathForAdd -OldValue $old -NewDir $Dir
    if ($null -eq $new) {
        Write-Step "用户 PATH 里已经有 $Dir 了,跳过。"
    } else {
        Set-UserPathRaw -NewValue $new
        Send-EnvironmentChangeBroadcast
        Write-Step "已把 $Dir 加进用户 PATH。"
    }

    if (Add-DirToProcessPath -Dir $Dir) {
        Write-Step "已刷新当前 PowerShell 的 PATH。"
    }
}

# ===================== 查找/下载可执行文件 =====================

function Find-LocalExe {
    param([string]$ScriptDir, [string]$SourceExe)
    if ($SourceExe) {
        if (-not (Test-Path -LiteralPath $SourceExe -PathType Leaf)) {
            throw "-SourceExe 指定的文件不存在:$SourceExe"
        }
        return (Resolve-Path -LiteralPath $SourceExe).Path
    }
    if ($ScriptDir) {
        $candidate = Join-Path $ScriptDir $ExeName
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return $candidate
        }
    }
    return $null
}

function Get-LatestReleaseDownloadUrl {
    param([string]$Repo)
    $api = "https://api.github.com/repos/$Repo/releases/latest"
    try {
        $resp = Invoke-RestMethod -Uri $api -UseBasicParsing -Headers @{ 'User-Agent' = 'lubancode-installer' }
    } catch {
        throw "查询最新 release 失败,检查一下网络,或者稍后重试。原始错误:$($_.Exception.Message)"
    }
    $asset = $resp.assets | Where-Object { $_.name -like '*windows-x64.zip' } | Select-Object -First 1
    if (-not $asset) {
        throw "最新 release 里没找到 windows-x64.zip 这个资产,下载不了。"
    }
    return $asset.browser_download_url
}

function Get-RemoteExe {
    param([string]$Repo, [string]$WorkDir)
    Write-Step "本地没找到 $ExeName,尝试从 GitHub 最新 release 下载..."
    $url = Get-LatestReleaseDownloadUrl -Repo $Repo
    $zipPath = Join-Path $WorkDir 'lubancode-download.zip'
    try {
        Invoke-WebRequest -Uri $url -OutFile $zipPath -UseBasicParsing
    } catch {
        throw "下载 $url 失败:$($_.Exception.Message)"
    }
    $extractDir = Join-Path $WorkDir 'extracted'
    try {
        Expand-Archive -LiteralPath $zipPath -DestinationPath $extractDir -Force
    } catch {
        throw "解压下载的 zip 失败:$($_.Exception.Message)"
    }
    $exe = Get-ChildItem -Path $extractDir -Filter $ExeName -Recurse | Select-Object -First 1
    if (-not $exe) {
        throw "下载的压缩包里没找到 $ExeName,包结构可能变了,联系维护者。"
    }
    return $exe.FullName
}

function Sync-OfficialDirectory {
    param(
        [string]$SourceExe,
        [string]$InstallDir,
        [string]$DirectoryName,
        [string]$DisplayName
    )
    if ([string]::IsNullOrWhiteSpace($DirectoryName) -or $DirectoryName.IndexOfAny([IO.Path]::GetInvalidFileNameChars()) -ge 0 `
        -or $DirectoryName.Contains('\') -or $DirectoryName.Contains('/')) {
        throw "官方资源目录名不合法:$DirectoryName"
    }

    $sourceDirectory = Join-Path (Split-Path -Parent $SourceExe) $DirectoryName
    if (-not (Test-Path -LiteralPath $sourceDirectory -PathType Container)) {
        Write-Host "提示:安装来源里没有 $DirectoryName 目录,保留现有$DisplayName 不动。" -ForegroundColor Yellow
        return
    }

    $installRoot = [IO.Path]::GetFullPath($InstallDir).TrimEnd('\')
    $destination = [IO.Path]::GetFullPath((Join-Path $installRoot $DirectoryName))
    if (-not $destination.StartsWith($installRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "$DisplayName 目标越出安装目录:$destination"
    }

    $staging = Join-Path $installRoot ('.' + $DirectoryName + '-new-' + [Guid]::NewGuid().ToString('N'))
    try {
        Copy-Item -LiteralPath $sourceDirectory -Destination $staging -Recurse -Force
        if (Test-Path -LiteralPath $destination) {
            Remove-Item -LiteralPath $destination -Recurse -Force
        }
        Move-Item -LiteralPath $staging -Destination $destination
    } finally {
        if (Test-Path -LiteralPath $staging) {
            Remove-Item -LiteralPath $staging -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
    Write-Step "已同步$DisplayName`:$destination"
}

function Sync-OfficialSkills {
    param(
        [string]$SourceExe,
        [string]$InstallDir
    )
    Sync-OfficialDirectory -SourceExe $SourceExe -InstallDir $InstallDir -DirectoryName 'skills' -DisplayName '官方技能'
}

function Sync-OfficialDocs {
    param(
        [string]$SourceExe,
        [string]$InstallDir
    )
    Sync-OfficialDirectory -SourceExe $SourceExe -InstallDir $InstallDir -DirectoryName 'docs' -DisplayName '官方文档'
}

# ===================== 主流程 =====================

function Invoke-Install {
    param(
        [string]$InstallDir,
        [string]$SourceExe,
        [switch]$SkipPath
    )

    if (-not $InstallDir) { $InstallDir = Get-DefaultInstallDir }
    Write-Step "安装目录:$InstallDir"

    $localExe = $null
    try {
        $localExe = Find-LocalExe -ScriptDir $PSScriptRoot -SourceExe $SourceExe
    } catch {
        Write-ErrStep "查找本地可执行文件失败:$($_.Exception.Message)"
        exit 1
    }

    $tempDownloadDir = $null
    $exeToInstall = $localExe
    if ($exeToInstall) {
        Write-Step "本地找到可执行文件:$exeToInstall"
    } else {
        $tempDownloadDir = Join-Path $env:TEMP ("lubancode-install-" + [Guid]::NewGuid().ToString('N'))
        try {
            New-Item -ItemType Directory -Path $tempDownloadDir -Force | Out-Null
            $exeToInstall = Get-RemoteExe -Repo $Repo -WorkDir $tempDownloadDir
        } catch {
            Write-ErrStep $_.Exception.Message
            if ($tempDownloadDir -and (Test-Path -LiteralPath $tempDownloadDir)) {
                Remove-Item -LiteralPath $tempDownloadDir -Recurse -Force -ErrorAction SilentlyContinue
            }
            exit 1
        }
    }

    try {
        New-Item -ItemType Directory -Path $InstallDir -Force | Out-Null
    } catch {
        Write-ErrStep "创建安装目录 $InstallDir 失败:$($_.Exception.Message)"
        exit 1
    }

    $destExe = Join-Path $InstallDir $ExeName
    try {
        $sourceFull = [IO.Path]::GetFullPath($exeToInstall)
        $destFull = [IO.Path]::GetFullPath($destExe)
        if (-not $sourceFull.Equals($destFull, [StringComparison]::OrdinalIgnoreCase)) {
            Copy-Item -LiteralPath $exeToInstall -Destination $destExe -Force
        }
        Sync-OfficialSkills -SourceExe $exeToInstall -InstallDir $InstallDir
        Sync-OfficialDocs -SourceExe $exeToInstall -InstallDir $InstallDir
    } catch {
        Write-ErrStep "同步程序、官方技能或文档失败(是不是有旧的 lubancode 进程占着文件?先关掉再重试):$($_.Exception.Message)"
        exit 1
    }

    if ($tempDownloadDir -and (Test-Path -LiteralPath $tempDownloadDir)) {
        Remove-Item -LiteralPath $tempDownloadDir -Recurse -Force -ErrorAction SilentlyContinue
    }

    if ($SkipPath) {
        Write-Step "已跳过 PATH 设置(-SkipPath)。"
    } else {
        try {
            Add-DirToUserPath -Dir $InstallDir
        } catch {
            Write-ErrStep "写入用户 PATH 失败:$($_.Exception.Message)(可以手动把 $InstallDir 加进环境变量,不影响 exe 本身已装好)"
            exit 1
        }
    }

    Write-Step "安装完成:$destExe"
    try {
        $verOutput = & $destExe --version
        Write-Host $verOutput -ForegroundColor Green
    } catch {
        Write-ErrStep "校验安装失败,跑 `"$destExe --version`" 报错:$($_.Exception.Message)"
        exit 1
    }

    if (-not $SkipPath) {
        Write-Host "现在可以直接运行 lubancode。" -ForegroundColor Green
    }
}

# 用 "." 号点调用(dot-source)本脚本时只加载函数、不执行安装,方便单测调用里面的纯逻辑函数。
if ($MyInvocation.InvocationName -ne '.') {
    Invoke-Install -InstallDir $InstallDir -SourceExe $SourceExe -SkipPath:$SkipPath
}
