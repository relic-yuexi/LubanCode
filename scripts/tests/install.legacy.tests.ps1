# Loaded by install.tests.ps1; package whitelist migration, no real installation.
$legacyRoot = Join-Path ([IO.Path]::GetTempPath()) ('lubancode-legacy-test-' + [Guid]::NewGuid().ToString('N'))
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
    Set-Content (Join-Path $newPackage 'notes.txt') 'not an official root file'
    Set-Content (Join-Path $legacyInstall '.env') 'user secret'
    Set-Content (Join-Path $newPackage '.env') 'not a user secret'
    Remove-Item -LiteralPath (Join-Path $newPackage 'skills/retired.md')
    $legacyTrees = Get-TreeMap $legacyInstall
    $newManifest = New-ManifestFromTree $newPackage
    $migrationPlan = Build-PackageFilePlan -NewMap (Get-ManifestFileMap $newManifest) `
        -DiskMap (Get-InstallDiskState -InstallRoot $legacyInstall -TreeMap $legacyTrees)
    $migrationResult = Invoke-ResourceApply -Plan $migrationPlan -NewManifest $newManifest -SourceRoot $newPackage `
        -InstallRoot $legacyInstall -TreeMap $legacyTrees -SourceMeta @{ repo = 'test/test' } `
        -InstallMode 'package-whitelist'
    Assert-FileText '旧安装迁移更新程序' (Join-Path $legacyInstall 'lubancode.exe') 'new executable'
    Assert-FileText '旧安装迁移更新官方技能' (Join-Path $legacyInstall 'skills/official.md') 'new official'
    Assert-FileText '白名单同名文件即使改过也更新' (Join-Path $legacyInstall 'skills/edited.md') 'new official edit'
    Assert-FileText '旧安装迁移保留自建技能' (Join-Path $legacyInstall 'skills/mine.md') 'my skill'
    Assert-FileText '旧安装迁移保留普通用户文件' (Join-Path $legacyInstall 'notes.txt') 'my note'
    Assert-FileText '清单误收 config.toml 也不能覆盖' (Join-Path $legacyInstall 'config.toml') 'user configuration'
    Assert-FileText '清单误收 .env 也不能覆盖' (Join-Path $legacyInstall '.env') 'user secret'
    Assert-FileText '覆盖前备份同名修改文件' (Join-Path $migrationResult.backupRoot 'skills/edited.md') 'my edit'
    Assert-Equal '新版不含的旧文件仍然保留' $true (Test-Path (Join-Path $legacyInstall 'skills/retired.md'))

    $repeatPlan = Build-PackageFilePlan -NewMap (Get-ManifestFileMap $newManifest) `
        -DiskMap (Get-InstallDiskState -InstallRoot $legacyInstall -TreeMap $legacyTrees)
    Assert-Equal '再次安装相同包无需覆盖或删除文件' 0 @($repeatPlan | Where-Object { $_.action -in @('replace', 'install-new', 'retire') }).Count

} finally {
    $legacyTemp = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\', '/')
    if ((Test-PathUnderRoot -Path ([IO.Path]::GetFullPath($legacyRoot)) -Root $legacyTemp) -and
        (Test-Path -LiteralPath $legacyRoot)) {
        Remove-Item -LiteralPath $legacyRoot -Recurse -Force
    }
}
