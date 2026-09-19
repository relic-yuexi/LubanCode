# Loaded by install.tests.ps1; downloads are mocked, never touches a real installation.
$legacyRoot = Join-Path ([IO.Path]::GetTempPath()) ('lubancode-legacy-test-' + [Guid]::NewGuid().ToString('N'))
$savedVersionFunction = ${function:Get-InstalledVersionFromExe}
$savedAssetsFunction = ${function:Get-LegacyReleaseAssets}
try {
    $oldPackage = Join-Path $legacyRoot 'old'
    $newPackage = Join-Path $legacyRoot 'new'
    $legacyInstall = Join-Path $legacyRoot 'installed'
    New-Item -ItemType Directory -Path (Join-Path $oldPackage 'skills') -Force | Out-Null
    Set-Content (Join-Path $oldPackage 'lubancode.exe') 'old executable'
    Set-Content (Join-Path $oldPackage 'skills/official.md') 'old official'
    Set-Content (Join-Path $oldPackage 'skills/edited.md') 'official original'
    Set-Content (Join-Path $oldPackage 'skills/retired.md') 'retired official'
    Set-Content (Join-Path $oldPackage 'config.toml') 'user configuration'
    Copy-Item -LiteralPath $oldPackage -Destination $legacyInstall -Recurse
    Copy-Item -LiteralPath $oldPackage -Destination $newPackage -Recurse
    Set-Content (Join-Path $legacyInstall 'skills/edited.md') 'my edit'
    Set-Content (Join-Path $legacyInstall 'skills/mine.md') 'my skill'
    Set-Content (Join-Path $legacyInstall 'notes.txt') 'my note'
    Set-Content (Join-Path $newPackage 'lubancode.exe') 'new executable'
    Set-Content (Join-Path $newPackage 'skills/official.md') 'new official'
    Set-Content (Join-Path $newPackage 'skills/edited.md') 'new official edit'
    Set-Content (Join-Path $newPackage 'config.toml') 'must never overwrite config'
    Remove-Item -LiteralPath (Join-Path $newPackage 'skills/retired.md')
    $legacyZip = Join-Path $legacyRoot 'official.zip'
    Compress-Archive -Path $oldPackage -DestinationPath $legacyZip
    $legacyDigest = 'sha256:' + (Get-FileSha256 $legacyZip)
    $legacyExeHash = Get-FileSha256 (Join-Path $legacyInstall 'lubancode.exe')

    $wrongDigestRejected = $false
    try {
        Read-VerifiedLegacyArchive -Archive $legacyZip -Digest ('sha256:' + ('0' * 64)) `
            -InstalledExeHash $legacyExeHash -ExtractRoot (Join-Path $legacyRoot 'bad') | Out-Null
    } catch { $wrongDigestRejected = $true }
    Assert-True '旧官方包摘要不符时拒绝建立基线' $wrongDigestRejected
    Assert-Equal '同版本不同 exe 不能冒充官方基线' $null (Read-VerifiedLegacyArchive `
        -Archive $legacyZip -Digest $legacyDigest -InstalledExeHash ('0' * 64) -ExtractRoot (Join-Path $legacyRoot 'mismatch'))

    function Get-InstalledVersionFromExe { param($ExePath) return '0.26.277' }
    function Get-LegacyReleaseAssets {
        param($Repo, $Version)
        return [PSCustomObject]@{ digest = $legacyDigest; browser_download_url = 'https://example.invalid/official.zip' }
    }
    function Invoke-WebRequest {
        param($Uri, $OutFile, [switch]$UseBasicParsing, $TimeoutSec)
        Copy-Item -LiteralPath $legacyZip -Destination $OutFile
    }
    $autoBaseline = Resolve-AutomaticBaseline -InstallRoot $legacyInstall -Repo 'test/test'
    Assert-True '无清单旧安装自动找回可信基线' ($null -ne $autoBaseline)
    Assert-Equal '自动基线不认领自建技能' $false ((Get-ManifestFileMap $autoBaseline).ContainsKey('skills/mine.md'))
    $legacyTrees = Get-TreeMap $legacyInstall
    $newManifest = New-ManifestFromTree $newPackage
    $migrationPlan = Build-FilePlan -NewMap (Get-ManifestFileMap $newManifest) `
        -OldMap (Get-ManifestFileMap $autoBaseline) `
        -DiskMap (Get-InstallDiskState -InstallRoot $legacyInstall -TreeMap $legacyTrees)
    Invoke-ResourceApply -Plan $migrationPlan -NewManifest $newManifest -SourceRoot $newPackage `
        -InstallRoot $legacyInstall -TreeMap $legacyTrees -SourceMeta @{ repo = 'test/test' } `
        -InstallMode 'automatic-legacy-baseline' | Out-Null
    Assert-FileText '旧安装迁移更新程序' (Join-Path $legacyInstall 'lubancode.exe') 'new executable'
    Assert-FileText '旧安装迁移更新官方技能' (Join-Path $legacyInstall 'skills/official.md') 'new official'
    Assert-FileText '旧安装迁移保留自行修改' (Join-Path $legacyInstall 'skills/edited.md') 'my edit'
    Assert-FileText '旧安装迁移保留自建技能' (Join-Path $legacyInstall 'skills/mine.md') 'my skill'
    Assert-FileText '旧安装迁移保留普通用户文件' (Join-Path $legacyInstall 'notes.txt') 'my note'
    Assert-FileText '清单误收 config.toml 也不能覆盖' (Join-Path $legacyInstall 'config.toml') 'user configuration'
    Assert-Equal '已退役官方文件清理' $false (Test-Path (Join-Path $legacyInstall 'skills/retired.md'))

    function Get-LegacyReleaseAssets { param($Repo, $Version) throw 'offline' }
    Assert-Equal '断网时不捏造基线' $null (Resolve-AutomaticBaseline -InstallRoot $legacyInstall -Repo 'test/test')
} finally {
    Set-Item Function:Get-InstalledVersionFromExe $savedVersionFunction
    Set-Item Function:Get-LegacyReleaseAssets $savedAssetsFunction
    Remove-Item Function:Invoke-WebRequest -ErrorAction SilentlyContinue
    $legacyTemp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\', '/')
    if ((Test-PathUnderRoot -Path ([IO.Path]::GetFullPath($legacyRoot)) -Root $legacyTemp) -and
        (Test-Path -LiteralPath $legacyRoot)) {
        Remove-Item -LiteralPath $legacyRoot -Recurse -Force
    }
}
