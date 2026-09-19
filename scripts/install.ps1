#Requires -Version 5.1
<#
    lubancode 安装脚本(Windows,PowerShell 5.1 兼容)

    用法:
        .\install.ps1                          自动模式:脚本同目录找得到 lubancode.exe 就本地装,
                                                 找不到就去 GitHub 最新 release 下载
        .\install.ps1 -SourceExe C:\a\b.exe     指定本地可执行文件路径(跳过自动查找/下载)
        .\install.ps1 -InstallDir D:\tools\lb   自定义安装目录(默认 %LOCALAPPDATA%\Programs\lubancode)
        .\install.ps1 -Scan                     扫描+预演:列出将替换/保留/备份/冲突项,不动安装
        .\install.ps1 -BackupOnly               完整备份现有受管内容+列盘面清单,不安装
        .\install.ps1 -Baseline C:\old\pkg      兼容旧参数;现在无需基线,统一按新包文件白名单覆盖
        .\install.ps1 -AllowUnknownReplace      兼容旧参数;仍逐文件覆盖,不会整目录删除
        .\install.ps1 -SkipPath                 不写用户 PATH(测试/CI 用,避免污染真实环境)

    不需要管理员权限——只动当前用户的安装目录和 HKCU 用户级 PATH,不碰系统级 PATH。

    资源覆盖策略:以新包清单内的具体文件路径为白名单。
      - 程序、README、LICENSE、声明、安装脚本及受管资源树内的包内文件逐项覆盖
      - 同名文件即使改过,也先备份再换新;内容相同则跳过
      - 目录只递归合并,新包没有的旧文件或自建文件一律保留
      - config.toml、.env、.lubancode/、.agents/ 等用户配置与数据不参与覆盖
      - 目录或链接挡住文件路径时报告冲突,不跟随链接写到安装根外
    无需旧清单或联网下载旧包。来源与安装目录相同时不搬自己。

    manifest 路径规则(与 scripts/generate_manifest.py、scripts/install_plan.py
    同一契约):只认包内相对路径;拒绝绝对路径、反斜杠、冒号(盘符/UNC/ADS)、
    空段、./.. 段、结尾点空格、Windows 保留名、大小写折叠碰撞。
#>
[CmdletBinding()]
param(
    [string]$InstallDir,
    [string]$SourceExe,
    [string]$Baseline,
    [switch]$AllowUnknownReplace,
    [switch]$Scan,
    [switch]$BackupOnly,
    [switch]$SkipPath
)

$ErrorActionPreference = 'Stop'

$Repo = 'relic-yuexi/LubanCode'

$AppName = 'lubancode'
$ExeName = 'lubancode.exe'

# 受管官方树(整棵由清单说了算);根级官方件(EXE/LICENSE/声明等)也入清单
$script:ManagedTrees = @('skills', 'docs', 'web', 'libexec', 'licenses', 'updater')
$script:RecordFiles = @('manifest.json', 'install-state.json')
$script:ManifestReservedNames = @(
    'CON', 'PRN', 'AUX', 'NUL',
    'COM1', 'COM2', 'COM3', 'COM4', 'COM5', 'COM6', 'COM7', 'COM8', 'COM9',
    'LPT1', 'LPT2', 'LPT3', 'LPT4', 'LPT5', 'LPT6', 'LPT7', 'LPT8', 'LPT9'
)

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

# ===================== 哈希与清单(与 generate_manifest.py 同一契约) =====================

function Get-FileSha256 {
    param([string]$Path)
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $stream = [IO.File]::OpenRead($Path)
        try {
            $hash = $sha.ComputeHash($stream)
        } finally {
            $stream.Dispose()
        }
    } finally {
        $sha.Dispose()
    }
    return ($hash | ForEach-Object { $_.ToString('x2') }) -join ''
}

function Split-ManifestRelative {
    <# 全路径 → 相对根的清单路径(正斜杠);Windows/Linux 分隔符通吃。 #>
    param([string]$FullPath, [string]$RootFull)
    $rel = $FullPath.Substring($RootFull.Length).TrimStart('\', '/')
    return ($rel -replace '\\', '/')
}

function Join-ManifestRelative {
    <# 清单路径(正斜杠)拼回平台真实路径。 #>
    param([string]$Root, [string]$RelPath)
    return (Join-Path $Root ($RelPath -replace '/', [IO.Path]::DirectorySeparatorChar))
}

function Test-PathUnderRoot {
    <# 严格子路径判断(带分隔符,不吃前缀巧合);根自身不算。 #>
    param([string]$Path, [string]$Root)
    $rootTrim = $Root.TrimEnd('\', '/')
    $sep = [IO.Path]::DirectorySeparatorChar
    return $Path.StartsWith($rootTrim + $sep, [StringComparison]::OrdinalIgnoreCase)
}

function Test-ManifestRelativePath {
    <#
        纯函数:清单相对路径合法性。拒绝绝对路径、反斜杠、冒号(盘符/UNC/ADS)、
        空段、./.. 段、结尾点空格、保留名、非法字符。返回 $true/$false。
    #>
    param([string]$RelPath)
    if ([string]::IsNullOrEmpty($RelPath)) { return $false }
    if ($RelPath.Length -gt 512) { return $false }
    if ($RelPath.StartsWith('/')) { return $false }
    if ($RelPath.Contains('\')) { return $false }
    if ($RelPath.Contains(':')) { return $false }
    foreach ($seg in ($RelPath -split '/')) {
        if ($seg -eq '' -or $seg -eq '.' -or $seg -eq '..') { return $false }
        if ($seg.EndsWith('.') -or $seg.EndsWith(' ')) { return $false }
        # 保留名连扩展名一起拒:com1.md 在 Windows 上照样惹祸
        $segStem = ($seg -split '\.', 2)[0].ToUpperInvariant()
        if (($script:ManifestReservedNames -contains $seg.ToUpperInvariant()) -or
            ($script:ManifestReservedNames -contains $segStem)) { return $false }
        foreach ($ch in $seg.ToCharArray()) {
            if ('<>:"|?*'.IndexOf($ch) -ge 0 -or [int]$ch -lt 0x20) { return $false }
        }
    }
    return $true
}

function Test-ManifestObject {
    <# 纯函数:清单对象整体校验,返回问题数组(空数组=通过)。 #>
    param($Manifest, [string]$Where)
    $problems = @()
    if ($null -eq $Manifest) {
        return @("$Where`:清单是空的")
    }
    if ($Manifest.schema -ne 1) { $problems += "$Where`:schema 不认($($Manifest.schema))" }
    if ($Manifest.algo -ne 'sha256') { $problems += "$Where`:algo 不认($($Manifest.algo))" }
    $files = @()
    if ($null -ne $Manifest.files) { $files = @($Manifest.files) }
    $seen = @{}
    foreach ($f in $files) {
        $p = [string]$f.path
        if (-not (Test-ManifestRelativePath -RelPath $p)) {
            $problems += "$Where`:路径不合法 $p"
            continue
        }
        $key = $p.ToLowerInvariant()
        if ($seen.ContainsKey($key)) {
            $problems += "$Where`:大小写碰撞 $($seen[$key]) 与 $p"
        }
        $seen[$key] = $p
        $sha = [string]$f.sha256
        if ($sha -notmatch '^[0-9a-f]{64}$') { $problems += "$Where`:sha256 不对 $p" }
    }
    return $problems
}

function Read-ManifestFile {
    <# 读+验;验不过直接 throw(调用方当致命错处理)。 #>
    param([string]$ManifestPath)
    $raw = Get-Content -LiteralPath $ManifestPath -Raw -Encoding UTF8
    $manifest = $null
    try {
        $manifest = $raw | ConvertFrom-Json
    } catch {
        throw "清单 JSON 解析失败:$ManifestPath($($_.Exception.Message))"
    }
    $problems = Test-ManifestObject -Manifest $manifest -Where $ManifestPath
    if ($problems.Count -gt 0) {
        throw ($problems -join '; ')
    }
    return $manifest
}

function New-ManifestFromTree {
    <# 来源没有官方清单时(本地开发目录等),现场给来源树建一张当新包清单。 #>
    param([string]$RootDir)
    $rootFull = [IO.Path]::GetFullPath($RootDir).TrimEnd('\', '/')
    $files = @()
    $stack = New-Object System.Collections.Generic.Stack[string]
    $stack.Push($rootFull)
    while ($stack.Count -gt 0) {
        $dir = $stack.Pop()
        foreach ($item in (Get-ChildItem -LiteralPath $dir -Force)) {
            $full = $item.FullName
            $rel = Split-ManifestRelative -FullPath $full -RootFull $rootFull
            if ($rel -eq 'manifest.json') { continue }
            $isReparse = (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)
            if ($isReparse) { throw "来源含链接/reparse,拒绝记账:$rel" }
            if ($item.PSIsContainer) {
                $stack.Push($full)
                continue
            }
            if (-not (Test-ManifestRelativePath -RelPath $rel)) {
                throw "来源文件路径不合法,拒绝记账:$rel"
            }
            $files += [PSCustomObject]@{
                path   = $rel
                size   = [int]$item.Length
                sha256 = (Get-FileSha256 -Path $full)
                role   = ''
            }
        }
    }
    # role 与 generate_manifest.py 同一套
    $roleTop = @('skills', 'docs', 'web', 'libexec', 'licenses')
    $roleRoot = @{
        'lubancode' = 'exe'; 'lubancode.exe' = 'exe'
        'LICENSE' = 'license'; 'THIRD_PARTY_NOTICES.md' = 'notices'
        'README.md' = 'readme'; 'README.en.md' = 'readme'
        'install.ps1' = 'installer'; 'uninstall.ps1' = 'installer'
        'install.sh' = 'installer'; 'install_plan.py' = 'installer'
    }
    $sorted = @($files | Sort-Object -Property path)
    for ($i = 0; $i -lt $sorted.Count; $i++) {
        $top = ($sorted[$i].path -split '/', 2)[0]
        if ($roleTop -contains $top) {
            $sorted[$i].role = $top
        } elseif ($roleRoot.ContainsKey($top)) {
            $sorted[$i].role = $roleRoot[$top]
        } else {
            $sorted[$i].role = 'asset'
        }
    }
    return [PSCustomObject]@{
        schema     = 1
        name       = 'lubancode'
        version    = $null
        platform   = $null
        channel    = $null
        algo       = 'sha256'
        file_count = $sorted.Count
        files      = $sorted
    }
}

function Get-ManifestFileMap {
    <# 清单 → 字典:相对路径 → @{path;sha256;size}。PowerShell 哈希表键本就不区分大小写。 #>
    param($Manifest)
    $map = @{}
    if ($null -eq $Manifest -or $null -eq $Manifest.files) { return $map }
    foreach ($f in @($Manifest.files)) {
        $map[[string]$f.path] = @{
            path   = [string]$f.path
            sha256 = [string]$f.sha256
            size   = $f.size
        }
    }
    return $map
}

# ===================== 盘面实况 =====================

function Get-TreeMap {
    param([string]$InstallDir)
    $map = @{}
    foreach ($tree in $script:ManagedTrees) {
        $map[$tree] = Join-Path $InstallDir $tree
    }
    return $map
}

function Get-InstallDiskState {
    <#
        扫每棵受管树(全递归但不跟进链接/reparse,防目录链接把枚举带出安装目录)
        + 安装根顶层文件(排除记录件)。返回 字典:相对路径 → 实况。
    #>
    param([string]$InstallRoot, [hashtable]$TreeMap)
    $disk = @{}
    $rootFull = [IO.Path]::GetFullPath($InstallRoot).TrimEnd('\', '/')
    if (-not (Test-Path -LiteralPath $rootFull -PathType Container)) { return $disk }

    foreach ($tree in $TreeMap.Keys) {
        $dest = [IO.Path]::GetFullPath($TreeMap[$tree]).TrimEnd('\', '/')
        if (-not (Test-PathUnderRoot -Path $dest -Root $rootFull)) { continue }
        if (-not (Test-Path -LiteralPath $dest -PathType Container)) { continue }
        $stack = New-Object System.Collections.Generic.Stack[string]
        $stack.Push($dest)
        while ($stack.Count -gt 0) {
            $dir = $stack.Pop()
            foreach ($item in (Get-ChildItem -LiteralPath $dir -Force)) {
                $rel = $tree + '/' + (Split-ManifestRelative -FullPath $item.FullName -RootFull $dest)
                $isReparse = (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)
                $disk[$rel] = @{
                    path    = $rel
                    abspath = $item.FullName
                    isDir   = [bool]$item.PSIsContainer
                    reparse = [bool]$isReparse
                    sha256  = $null
                }
                if ($item.PSIsContainer -and -not $isReparse) {
                    $stack.Push($item.FullName)
                }
            }
        }
    }

    foreach ($item in @(Get-ChildItem -LiteralPath $rootFull -File -Force)) {
        if ($script:RecordFiles -contains $item.Name) { continue }
        $isReparse = (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)
        $disk[$item.Name] = @{
            path    = $item.Name
            abspath = $item.FullName
            isDir   = $false
            reparse = [bool]$isReparse
            sha256  = $null
        }
    }
    return $disk
}

function Get-DiskHash {
    param($Entry)
    if ($null -eq $Entry.sha256 -and -not $Entry.reparse -and -not $Entry.isDir) {
        try {
            $Entry.sha256 = Get-FileSha256 -Path $Entry.abspath
        } catch {
            $Entry.reparse = $true
        }
    }
    return $Entry.sha256
}

function Get-DestPathFor {
    <# 清单相对路径 → 安装目录内绝对路径;顺带做越界与合法性检查。 #>
    param([string]$RelPath, [string]$InstallRoot, [hashtable]$TreeMap)
    if (-not (Test-ManifestRelativePath -RelPath $RelPath)) {
        throw "路径不合法,拒绝落地:$RelPath"
    }
    $rootFull = [IO.Path]::GetFullPath($InstallRoot).TrimEnd('\', '/')
    $top = ($RelPath -split '/', 2)[0]
    if ($TreeMap.ContainsKey($top) -and $RelPath.Contains('/')) {
        $rest = ($RelPath -split '/', 2)[1]
        $dest = Join-ManifestRelative -Root $TreeMap[$top] -RelPath $rest
    } else {
        $dest = Join-ManifestRelative -Root $rootFull -RelPath $RelPath
    }
    $destFull = [IO.Path]::GetFullPath($dest)
    if (-not (Test-PathUnderRoot -Path $destFull -Root $rootFull)) {
        throw "目标越出安装目录:$RelPath -> $destFull"
    }
    $parent = Split-Path -Parent $destFull
    while ($parent -and (Test-PathUnderRoot -Path $parent -Root $rootFull)) {
        if (Test-Path -LiteralPath $parent) {
            $parentItem = Get-Item -LiteralPath $parent -Force
            if (-not $parentItem.PSIsContainer -or
                (($parentItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
                throw "目标父路径不是普通目录,拒绝写入:$parent"
            }
        }
        $parent = Split-Path -Parent $parent
    }
    return $destFull
}

# ===================== 决策表(纯函数,单测主阵地) =====================

function Build-FilePlan {
    <#
        纯决策:new/old/disk 三张表进,动作清单出(与 install_plan.py 同一张表):
          replace / skip-current / install-new / missing-kept /
          conflict-modified / conflict-collision / conflict-reparse / conflict-kind /
          retire / keep-modified-retired / keep-unknown
        表驱动约定:new=新包清单 old=上次官方清单 disk=盘面实况;
        N/O/D=在不在,hd=盘面哈希,ho=旧清单哈希,hn=新清单哈希。
    #>
    param(
        [hashtable]$NewMap,
        [hashtable]$OldMap,
        [hashtable]$DiskMap
    )
    $universe = @{}
    foreach ($k in @($NewMap.Keys)) {
        if (-not $universe.ContainsKey($k)) { $universe[$k] = @{} }
        $universe[$k]['new'] = $NewMap[$k]
    }
    foreach ($k in @($OldMap.Keys)) {
        if (-not $universe.ContainsKey($k)) { $universe[$k] = @{} }
        $universe[$k]['old'] = $OldMap[$k]
    }
    foreach ($k in @($DiskMap.Keys)) {
        if (-not $universe.ContainsKey($k)) { $universe[$k] = @{} }
        $universe[$k]['disk'] = $DiskMap[$k]
    }

    $plan = @()
    foreach ($k in @($universe.Keys | Sort-Object)) {
        $cell = $universe[$k]
        $n = $cell['new']
        $o = $cell['old']
        $d = $cell['disk']

        $entry = [ordered]@{ path = ''; action = ''; reason = ''; backup = $false }
        if ($null -ne $n) { $entry.path = [string]$n['path'] }
        elseif ($null -ne $o) { $entry.path = [string]$o['path'] }
        else { $entry.path = [string]$d['path'] }

        # 用户配置和用户数据从不参与安装,即使来源或旧清单误收了它们。
        if ($entry.path -match '^(config\.toml|\.env|\.lubancode|\.agents)(/|$)') {
            $entry.action = 'keep-user-data'
            $entry.reason = '用户配置或数据,不写不删'
            $plan += [PSCustomObject]$entry
            continue
        }

        if ($null -ne $d -and ($d['reparse'] -or $d['isDir'])) {
            # 普通容器目录(清单没把它当文件)不进计划;链接/reparse 即便清单
            # 不认识也点名留观(绝不去动);只有清单要文件的路径被目录/链接
            # 挡住才是 conflict-kind / conflict-reparse。
            if ($null -eq $n -and $null -eq $o -and -not $d['reparse']) {
                continue
            }
            if ($d['isDir'] -and -not $d['reparse']) {
                $entry.action = 'conflict-kind'
                $entry.reason = '盘面是目录,不写不删'
            } else {
                $entry.action = 'conflict-reparse'
                $entry.reason = '盘面是链接/reparse,不写不删'
            }
            $plan += [PSCustomObject]$entry
            continue
        }
        $hd = $null
        if ($null -ne $d) { $hd = Get-DiskHash -Entry $d }

        if ($null -ne $n) {
            if ($null -eq $d) {
                if ($null -ne $o) {
                    $entry.action = 'missing-kept'
                    $entry.reason = '官方件本地已删,不悄悄复活'
                } else {
                    $entry.action = 'install-new'
                    $entry.reason = '新版新增'
                }
            } elseif ($null -ne $o) {
                if ($hd -eq $o['sha256']) {
                    if ($n['sha256'] -eq $hd) {
                        $entry.action = 'skip-current'
                        $entry.reason = '已是新版内容'
                    } else {
                        $entry.action = 'replace'
                        $entry.reason = '官方未改,换新'
                        $entry.backup = $true
                    }
                } else {
                    $entry.action = 'conflict-modified'
                    $entry.reason = '本地改过官方件,原件保留+备份'
                    $entry.backup = $true
                }
            } else {
                $entry.action = 'conflict-collision'
                $entry.reason = '未知文件撞上新版同路径,保留+备份'
                $entry.backup = $true
            }
        } elseif ($null -ne $d) {
            if ($null -ne $o) {
                if ($hd -eq $o['sha256']) {
                    $entry.action = 'retire'
                    $entry.reason = '官方新版已删且本地未改,随旧版退役'
                    $entry.backup = $true
                } else {
                    $entry.action = 'keep-modified-retired'
                    $entry.reason = '本地改过且新版已删,保留'
                }
            } else {
                $entry.action = 'keep-unknown'
                $entry.reason = '用户/未知文件,保留'
            }
        } else {
            continue  # 盘面无痕且不在新版:无事可做
        }
        $plan += [PSCustomObject]$entry
    }
    return @($plan)
}

# ===================== 备份 =====================

function New-BackupRoot {
    param([string]$InstallRoot)
    $stamp = (Get-Date).ToUniversalTime().ToString('yyyyMMddTHHmmssZ')
    $suffix = -join (1..8 | ForEach-Object { '{0:x}' -f (Get-Random -Maximum 16) })
    $root = Join-Path (Join-Path $InstallRoot 'backups') ($stamp + '-' + $suffix)
    New-Item -ItemType Directory -Path $root -Force | Out-Null
    return $root
}

function Invoke-FullBackup {
    <# 无基线时的完整备份:受管树全量 + 安装根顶层文件,原样进备份。 #>
    param([string]$InstallRoot, [hashtable]$TreeMap)
    $backupRoot = New-BackupRoot -InstallRoot $InstallRoot
    foreach ($tree in $TreeMap.Keys) {
        $dest = $TreeMap[$tree]
        if (-not (Test-Path -LiteralPath $dest -PathType Container)) { continue }
        Copy-Item -LiteralPath $dest -Destination (Join-Path $backupRoot $tree) -Recurse -Force
    }
    foreach ($item in @(Get-ChildItem -LiteralPath $InstallRoot -File -Force)) {
        if ($script:RecordFiles -contains $item.Name) { continue }
        Copy-Item -LiteralPath $item.FullName -Destination (Join-Path $backupRoot $item.Name) -Force
    }
    return $backupRoot
}

# ===================== 记录件 =====================

function Read-InstallBaseline {
    <# 上次可信官方清单:先认 install-state.json,再退根下平铺旧清单。 #>
    param([string]$InstallRoot)
    $statePath = Join-Path $InstallRoot 'install-state.json'
    if (Test-Path -LiteralPath $statePath -PathType Leaf) {
        try {
            $state = Get-Content -LiteralPath $statePath -Raw -Encoding UTF8 | ConvertFrom-Json
            if ($null -ne $state.manifest) {
                $problems = Test-ManifestObject -Manifest $state.manifest -Where $statePath
                if ($problems.Count -eq 0) { return $state.manifest }
            }
        } catch {
            Write-Host "警告:install-state.json 读不了($($_.Exception.Message)),当无基线处理。" -ForegroundColor Yellow
        }
    }
    $legacy = Join-Path $InstallRoot 'manifest.json'
    if (Test-Path -LiteralPath $legacy -PathType Leaf) {
        try {
            return (Read-ManifestFile -ManifestPath $legacy)
        } catch {
            Write-Host "警告:旧版平铺清单读不了($($_.Exception.Message)),当无基线处理。" -ForegroundColor Yellow
        }
    }
    return $null
}

function Get-InstalledVersionFromExe {
    param([string]$ExePath)
    if (-not (Test-Path -LiteralPath $ExePath -PathType Leaf)) { return $null }
    # 非 Windows:不可执行文件交给 pwsh 会兜到 xdg-open(又慢又吵),先看执行位
    if ($env:OS -ne 'Windows_NT') {
        try {
            $mode = [System.IO.File]::GetUnixFileMode($ExePath)
            if (($mode -band [System.IO.UnixFileMode]::UserExecute) -eq 0) { return $null }
        } catch {
            return $null
        }
    }
    try {
        $out = & $ExePath --version 2>$null
        foreach ($tok in ("$out" -split '\s+')) {
            if ($tok -match '^[0-9]') { return $tok }
        }
    } catch {
    }
    return $null
}

function Write-InstallRecords {
    <# manifest.json(官方原件逐字节照抄;现场生成的落转义文本) + install-state.json。 #>
    param(
        $NewManifest,
        [string]$SourceRoot,
        [string]$InstallRoot,
        [hashtable]$SourceMeta,
        [string]$InstallMode,
        [array]$Conflicts
    )
    $srcManifest = Join-Path $SourceRoot 'manifest.json'
    $dstManifest = Join-Path $InstallRoot 'manifest.json'
    $provenance = 'generated-from-source'
    if (Test-Path -LiteralPath $srcManifest -PathType Leaf) {
        $srcFull = [IO.Path]::GetFullPath($srcManifest)
        $dstFull = [IO.Path]::GetFullPath($dstManifest)
        $isSame = $srcFull.Equals($dstFull, [StringComparison]::OrdinalIgnoreCase) -and
            (Test-Path -LiteralPath $dstFull -PathType Leaf)
        if (-not $isSame) {
            Copy-Item -LiteralPath $srcManifest -Destination $dstManifest -Force
        }
        $provenance = 'official-package'
    } else {
        $json = ConvertTo-Json -InputObject $NewManifest -Depth 8
        Set-Content -LiteralPath $dstManifest -Value $json -Encoding UTF8
    }

    $version = $NewManifest.version
    if (-not $version) { $version = Get-InstalledVersionFromExe -ExePath (Join-Path $InstallRoot $ExeName) }
    $state = [ordered]@{
        schema              = 1
        installed_at_utc    = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
        version             = $version
        platform            = $NewManifest.platform
        channel             = $NewManifest.channel
        source              = [ordered]@{
            repo         = $SourceMeta['repo']
            release_tag  = $SourceMeta['release_tag']
            asset_name   = $SourceMeta['asset_name']
            download_url = $SourceMeta['download_url']
        }
        install_mode        = $InstallMode
        manifest_provenance = $provenance
        installer           = 'install.ps1'
        manifest            = $NewManifest
        pending_conflicts   = @($Conflicts)
    }
    $stateJson = ConvertTo-Json -InputObject $state -Depth 8
    Set-Content -LiteralPath (Join-Path $InstallRoot 'install-state.json') -Value $stateJson -Encoding UTF8
}

# ===================== 执行与报告 =====================

function Invoke-ResourceApply {
    <# 按计划落地:备份 → 装/换 → 退役 → 空目录收尾 → 记档。返回摘要。 #>
    param(
        [array]$Plan,
        $NewManifest,
        [string]$SourceRoot,
        [string]$InstallRoot,
        [hashtable]$TreeMap,
        [hashtable]$SourceMeta,
        [string]$InstallMode
    )
    $backupRoot = New-BackupRoot -InstallRoot $InstallRoot
    $conflicts = @()
    $retiredParents = @()
    $sourceRootFull = [IO.Path]::GetFullPath($SourceRoot).TrimEnd('\', '/')

    foreach ($e in $Plan) {
        $needDest = ($e.backup -or $e.action -in @('install-new', 'replace', 'retire'))
        if (-not $needDest) {
            if ($e.action -like 'conflict*' -or $e.action -eq 'keep-modified-retired') {
                $conflicts += [ordered]@{
                    path   = $e.path
                    kind   = $e.action
                    backup = ('backups/' + (Split-Path -Leaf $backupRoot))
                }
            }
            continue
        }
        $dst = Get-DestPathFor -RelPath $e.path -InstallRoot $InstallRoot -TreeMap $TreeMap

        if ($e.backup -and (Test-Path -LiteralPath $dst -PathType Leaf)) {
            $item = Get-Item -LiteralPath $dst -Force
            if ((($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0)) {
                $backupDest = Join-ManifestRelative -Root $backupRoot -RelPath $e.path
                New-Item -ItemType Directory -Path (Split-Path -Parent $backupDest) -Force | Out-Null
                Copy-Item -LiteralPath $dst -Destination $backupDest -Force
            }
        }

        switch ($e.action) {
            'install-new' {
                $src = Join-ManifestRelative -Root $sourceRootFull -RelPath $e.path
                if ((Test-Path -LiteralPath $dst -PathType Leaf) -and
                    (([IO.Path]::GetFullPath($src)).Equals($dst, [StringComparison]::OrdinalIgnoreCase))) {
                    break  # 同目录安装,不搬自己
                }
                New-Item -ItemType Directory -Path (Split-Path -Parent $dst) -Force | Out-Null
                Copy-Item -LiteralPath $src -Destination $dst -Force
            }
            'replace' {
                $src = Join-ManifestRelative -Root $sourceRootFull -RelPath $e.path
                if (([IO.Path]::GetFullPath($src)).Equals($dst, [StringComparison]::OrdinalIgnoreCase)) {
                    break  # 同目录安装,不搬自己
                }
                New-Item -ItemType Directory -Path (Split-Path -Parent $dst) -Force | Out-Null
                Copy-Item -LiteralPath $src -Destination $dst -Force
            }
            'retire' {
                if (Test-Path -LiteralPath $dst -PathType Leaf) {
                    Remove-Item -LiteralPath $dst -Force
                    $retiredParents += (Split-Path -Parent $dst)
                }
            }
            default {
                if ($e.action -like 'conflict*' -or $e.action -eq 'keep-modified-retired') {
                    $conflicts += [ordered]@{
                        path   = $e.path
                        kind   = $e.action
                        backup = ('backups/' + (Split-Path -Leaf $backupRoot))
                    }
                }
            }
        }
    }

    # 退役文件的空父目录收尾:只删真正删空的,删到安装根或非空为止
    $rootFull = [IO.Path]::GetFullPath($InstallRoot).TrimEnd('\', '/')
    foreach ($parent in @($retiredParents | Sort-Object -Property Length -Descending | Select-Object -Unique)) {
        $cur = $parent
        for ($i = 0; $i -lt 64 -and $cur; $i++) {
            $curFull = [IO.Path]::GetFullPath($cur).TrimEnd('\', '/')
            # 严格子路径(带分隔符):绝不能把安装根自身删了
            if (-not (Test-PathUnderRoot -Path $curFull -Root $rootFull)) { break }
            if (-not (Test-Path -LiteralPath $curFull -PathType Container)) { break }
            if (@(Get-ChildItem -LiteralPath $curFull -Force).Count -gt 0) { break }
            try {
                [IO.Directory]::Delete($curFull, $false)
            } catch {
                break
            }
            $cur = Split-Path -Parent $curFull
        }
    }

    Write-InstallRecords -NewManifest $NewManifest -SourceRoot $sourceRootFull -InstallRoot $InstallRoot `
        -SourceMeta $SourceMeta -InstallMode $InstallMode -Conflicts $conflicts

    return @{
        backupRoot = $backupRoot
        conflicts  = $conflicts
    }
}

function Invoke-WholesaleReplace {
    <# 无基线+显式确认(-AllowUnknownReplace)时的整目录替换:旧脚本行为,但同源同目标不删自己。 #>
    param([string]$SourceRoot, [string]$InstallRoot, [hashtable]$TreeMap)
    $sourceRootFull = [IO.Path]::GetFullPath($SourceRoot).TrimEnd('\', '/')
    $installRootFull = [IO.Path]::GetFullPath($InstallRoot).TrimEnd('\', '/')

    foreach ($tree in $script:ManagedTrees) {
        $srcTree = Join-Path $sourceRootFull $tree
        if (-not (Test-Path -LiteralPath $srcTree -PathType Container)) { continue }
        $dst = [IO.Path]::GetFullPath($TreeMap[$tree]).TrimEnd('\', '/')
        if ($srcTree.Equals($dst, [StringComparison]::OrdinalIgnoreCase)) {
            Write-Host "[plan] 同目录安装,跳过 $tree"
            continue
        }
        $staging = Join-Path (Split-Path -Parent $dst) ('.' + $tree + '-new-' + [Guid]::NewGuid().ToString('N'))
        try {
            Copy-Item -LiteralPath $srcTree -Destination $staging -Recurse -Force
            if (Test-Path -LiteralPath $dst) {
                Remove-Item -LiteralPath $dst -Recurse -Force
            }
            Move-Item -LiteralPath $staging -Destination $dst
        } finally {
            if (Test-Path -LiteralPath $staging) {
                Remove-Item -LiteralPath $staging -Recurse -Force -ErrorAction SilentlyContinue
            }
        }
        Write-Host "[plan] 整目录换入 $dst"
    }

    foreach ($item in @(Get-ChildItem -LiteralPath $sourceRootFull -File -Force)) {
        if ($script:RecordFiles -contains $item.Name) { continue }
        $dst = Join-Path $installRootFull $item.Name
        if ((Test-Path -LiteralPath $dst -PathType Leaf) -and
            ($item.FullName.Equals([IO.Path]::GetFullPath($dst), [StringComparison]::OrdinalIgnoreCase))) {
            continue
        }
        Copy-Item -LiteralPath $item.FullName -Destination $dst -Force
    }
}

function Show-PlanReport {
    param([array]$Plan, $NewManifest, $OldManifest)
    $total = 0
    if ($null -ne $NewManifest -and $null -ne $NewManifest.files) { $total = @($NewManifest.files).Count }
    $baseline = '无(旧安装没有清单)'
    if ($null -ne $OldManifest) { $baseline = '有(上次官方清单)' }
    Write-Host "预演:新版 $total 个官方文件;基线:$baseline"
    $counts = @{}
    $hasConflict = $false
    foreach ($e in $Plan) {
        Write-Host "[plan] $($e.action) $($e.path) ($($e.reason))"
        if (-not $counts.ContainsKey($e.action)) { $counts[$e.action] = 0 }
        $counts[$e.action]++
        if ($e.action -like 'conflict*' -or $e.action -in @('keep-modified-retired', 'missing-kept')) { $hasConflict = $true }
    }
    $summary = ($counts.Keys | Sort-Object | ForEach-Object { "$($_)=$($counts[$_])" }) -join ', '
    if ([string]::IsNullOrEmpty($summary)) { $summary = '无事可做' }
    Write-Host "合计:$summary"
    if ($hasConflict) {
        Write-Host "注意:存在冲突/本地差异项(见上)。执行安装时这些路径一律保留原件并备份,官方新版不落这些路径。" -ForegroundColor Yellow
    }
}

function Show-NeedsReview {
    param([hashtable]$DiskMap, [hashtable]$NewMap)
    $collisions = @()
    foreach ($k in @($NewMap.Keys)) {
        if ($DiskMap.ContainsKey($k)) { $collisions += $DiskMap[$k]['path'] }
    }
    Write-Host "needs-review:旧安装没有清单(基线),且新包路径与盘面相撞 $($collisions.Count) 项:" -ForegroundColor Yellow
    foreach ($p in @($collisions | Sort-Object | Select-Object -First 50)) {
        Write-Host "  相撞:$p"
    }
    Write-Host "两条路:"
    Write-Host "  1) 拿同版本可信原包(解压成目录或直接给 zip)重跑:.\install.ps1 -Baseline <原包路径>"
    Write-Host "  2) 确认安装目录里没有要保的东西后:.\install.ps1 -AllowUnknownReplace(先完整备份再整目录替换)"
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
    return @{ url = $asset.browser_download_url; asset = $asset.name; tag = $resp.tag_name }
}

function Get-RemoteExe {
    param([string]$Repo, [string]$WorkDir)
    Write-Step "本地没找到 $ExeName,尝试从 GitHub 最新 release 下载..."
    $meta = Get-LatestReleaseDownloadUrl -Repo $Repo
    $zipPath = Join-Path $WorkDir 'lubancode-download.zip'
    try {
        Invoke-WebRequest -Uri $meta.url -OutFile $zipPath -UseBasicParsing
    } catch {
        throw "下载 $($meta.url) 失败:$($_.Exception.Message)"
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
    return @{ exe = $exe.FullName; url = $meta.url; asset = $meta.asset; tag = $meta.tag }
}

function Resolve-BaselineManifest {
    <# -Baseline 给目录就用;给 zip 就解压到临时目录再用。返回 manifest 或 $null。 #>
    param([string]$Baseline)
    if (-not $Baseline) { return $null }
    if (Test-Path -LiteralPath $Baseline -PathType Container) {
        $dir = $Baseline
    } elseif ($Baseline -like '*.zip' -and (Test-Path -LiteralPath $Baseline -PathType Leaf)) {
        $dir = Join-Path $env:TEMP ("lubancode-baseline-" + [Guid]::NewGuid().ToString('N'))
        Expand-Archive -LiteralPath $Baseline -DestinationPath $dir -Force
        # zip 里通常还裹一层 lubancode/;找带 exe 的那一层
        $inner = Get-ChildItem -Path $dir -Filter $ExeName -Recurse | Select-Object -First 1
        if ($inner) { $dir = Split-Path -Parent $inner.FullName }
    } else {
        throw "-Baseline 指定的路径不存在,或既不是目录也不是 zip:$Baseline"
    }
    $manifestPath = Join-Path $dir 'manifest.json'
    if (Test-Path -LiteralPath $manifestPath -PathType Leaf) {
        return (Read-ManifestFile -ManifestPath $manifestPath)
    }
    return (New-ManifestFromTree -RootDir $dir)
}

function Build-PackageFilePlan {
    # 新包的文件路径就是覆盖白名单;目录只合并,不删除包外文件。
    param([hashtable]$NewMap, [hashtable]$DiskMap)
    $allowed = @{}
    $current = @{}
    $rootFiles = @('lubancode.exe', 'LICENSE', 'THIRD_PARTY_NOTICES.md',
        'README.md', 'README.en.md', 'install.ps1', 'uninstall.ps1')
    foreach ($key in $NewMap.Keys) {
        $path = [string]$NewMap[$key].path
        $top = ($path -split '/', 2)[0]
        if (($rootFiles -contains $path) -or
            ($path.Contains('/') -and $script:ManagedTrees -contains $top)) {
            $allowed[$key] = $NewMap[$key]
            # 将同名现有普通文件作为替换对象,不要求旧版清单。
            if ($DiskMap.ContainsKey($key) -and -not $DiskMap[$key].isDir -and -not $DiskMap[$key].reparse) {
                $current[$key] = @{ path = $path; sha256 = (Get-DiskHash $DiskMap[$key]) }
            }
        }
    }
    # OldMap 仅含新包同名路径,所以不会产生 retire 或 missing-kept。
    return (Build-FilePlan -NewMap $allowed -OldMap $current -DiskMap $DiskMap)
}

# ===================== 主流程 =====================

function Invoke-Install {
    param(
        [string]$InstallDir,
        [string]$SourceExe,
        [string]$Baseline,
        [switch]$AllowUnknownReplace,
        [switch]$Scan,
        [switch]$BackupOnly,
        [switch]$SkipPath
    )

    if (-not $InstallDir) { $InstallDir = Get-DefaultInstallDir }
    $installRootFull = [IO.Path]::GetFullPath($InstallDir).TrimEnd('\', '/')
    Write-Step "安装目录:$installRootFull"

    $treeMap = Get-TreeMap -InstallDir $installRootFull
    $installExists = (Test-Path -LiteralPath $installRootFull -PathType Container)

    # ---- BackupOnly:纯本地动作,不需要来源包,不安装 ----
    if ($BackupOnly) {
        if (-not $installExists) {
            Write-Step "安装目录不存在,无事可备份。"
            return
        }
        $backupRoot = Invoke-FullBackup -InstallRoot $installRootFull -TreeMap $treeMap
        Write-Step "完整备份完成:$backupRoot"
        $disk = Get-InstallDiskState -InstallRoot $installRootFull -TreeMap $treeMap
        foreach ($k in @($disk.Keys | Sort-Object)) {
            Write-Host "[backup-only] 盘面文件 $($disk[$k]['path'])"
        }
        return
    }

    # ---- 找来源 ----
    $localExe = $null
    try {
        $localExe = Find-LocalExe -ScriptDir $PSScriptRoot -SourceExe $SourceExe
    } catch {
        Write-ErrStep "查找本地可执行文件失败:$($_.Exception.Message)"
        exit 1
    }

    $sourceMeta = @{ repo = $Repo; release_tag = $null; asset_name = $null; download_url = $null }
    $installMode = 'local-dir'
    $tempDownloadDir = $null
    $exeToInstall = $localExe
    if ($exeToInstall) {
        Write-Step "本地找到可执行文件:$exeToInstall"
    } else {
        if ($Scan) {
            # 预演不下载:只报要下哪个包
            try {
                $meta = Get-LatestReleaseDownloadUrl -Repo $Repo
                Write-Step "将下载:$($meta.url)"
            } catch {
                Write-ErrStep $_.Exception.Message
                exit 1
            }
            Write-Step "预演到此为止(未下载包,无清单可比对)。"
            return
        }
        $tempDownloadDir = Join-Path $env:TEMP ("lubancode-install-" + [Guid]::NewGuid().ToString('N'))
        try {
            New-Item -ItemType Directory -Path $tempDownloadDir -Force | Out-Null
            $remote = Get-RemoteExe -Repo $Repo -WorkDir $tempDownloadDir
            $exeToInstall = $remote.exe
            $sourceMeta['release_tag'] = $remote.tag
            $sourceMeta['asset_name'] = $remote.asset
            $sourceMeta['download_url'] = $remote.url
            $installMode = 'github-latest'
        } catch {
            Write-ErrStep $_.Exception.Message
            if ($tempDownloadDir -and (Test-Path -LiteralPath $tempDownloadDir)) {
                Remove-Item -LiteralPath $tempDownloadDir -Recurse -Force -ErrorAction SilentlyContinue
            }
            exit 1
        }
    }

    $sourceRoot = (Split-Path -Parent $exeToInstall)
    $sourceRootFull = [IO.Path]::GetFullPath($sourceRoot).TrimEnd('\', '/')

    # ---- 新包清单 ----
    $newManifest = $null
    $sourceManifestPath = Join-Path $sourceRootFull 'manifest.json'
    if (Test-Path -LiteralPath $sourceManifestPath -PathType Leaf) {
        try {
            $newManifest = Read-ManifestFile -ManifestPath $sourceManifestPath
        } catch {
            Write-ErrStep "来源 manifest.json 不合格,拒绝安装:$($_.Exception.Message)"
            if ($tempDownloadDir -and (Test-Path -LiteralPath $tempDownloadDir)) {
                Remove-Item -LiteralPath $tempDownloadDir -Recurse -Force -ErrorAction SilentlyContinue
            }
            exit 1
        }
    } else {
        Write-Host "提示:来源没有官方 manifest.json(本地开发目录?),按来源现状现场建清单。" -ForegroundColor Yellow
        $newManifest = New-ManifestFromTree -RootDir $sourceRootFull
    }

    # ---- 来源与目标同一目录:文件已在位,不搬自己,只补记档 ----
    if ($sourceRootFull.Equals($installRootFull, [StringComparison]::OrdinalIgnoreCase)) {
        if ($Scan) {
            Write-Step "同目录安装,预演无事可做(文件已在位)。"
            return
        }
        Write-InstallRecords -NewManifest $newManifest -SourceRoot $sourceRootFull -InstallRoot $installRootFull `
            -SourceMeta $sourceMeta -InstallMode 'same-dir' -Conflicts @()
        Write-Step "同目录安装:文件已在位,不重复搬动,已补记档。"
        $destExe = Join-Path $installRootFull $ExeName
        Write-Step "安装完成:$destExe"
        return
    }

    # 旧参数继续接受,但不再整目录替换,也无需下载旧版包。
    if ($Baseline -or $AllowUnknownReplace) {
        Write-Step '现在统一按新包文件白名单覆盖;旧参数无需再传,包外文件一律保留。'
    }
    $newMap = Get-ManifestFileMap -Manifest $newManifest
    $disk = Get-InstallDiskState -InstallRoot $installRootFull -TreeMap $treeMap
    $plan = Build-PackageFilePlan -NewMap $newMap -DiskMap $disk
    if ($Scan) {
        Show-PlanReport -Plan $plan -NewManifest $newManifest -OldManifest $null
        Write-Step '预演结束,未动任何文件。'
        return
    }
    New-Item -ItemType Directory -Path $installRootFull -Force | Out-Null
    try {
        $result = Invoke-ResourceApply -Plan $plan -NewManifest $newManifest -SourceRoot $sourceRootFull `
            -InstallRoot $installRootFull -TreeMap $treeMap -SourceMeta $sourceMeta -InstallMode 'package-whitelist'
    } catch {
        Write-ErrStep "安装失败(若程序占用,请关闭后重试):$($_.Exception.Message)"
        exit 1
    } finally {
        if ($tempDownloadDir -and (Test-Path -LiteralPath $tempDownloadDir)) {
            Remove-Item -LiteralPath $tempDownloadDir -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
    Write-Step "白名单文件已应用,包外文件保留。备份根:$($result.backupRoot)"
    if ($result.conflicts.Count -gt 0) {
        foreach ($conflict in $result.conflicts) {
            Write-Host "路径冲突,未覆盖:$($conflict.path) ($($conflict.kind))" -ForegroundColor Yellow
        }
        Write-ErrStep '部分路径被目录或链接占用,安装未完成;请处理冲突后重试。'
        exit 3
    }

    if ($SkipPath) {
        Write-Step "已跳过 PATH 设置(-SkipPath)。"
    } else {
        try {
            Add-DirToUserPath -Dir $installRootFull
        } catch {
            Write-ErrStep "写入用户 PATH 失败:$($_.Exception.Message)(可以手动把 $installRootFull 加进环境变量,不影响 exe 本身已装好)"
            exit 1
        }
    }

    $destExe = Join-Path $installRootFull $ExeName
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
    Invoke-Install -InstallDir $InstallDir -SourceExe $SourceExe -Baseline $Baseline `
        -AllowUnknownReplace:$AllowUnknownReplace -Scan:$Scan -BackupOnly:$BackupOnly -SkipPath:$SkipPath
}
