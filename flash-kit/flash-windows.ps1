# 重师大 AI 工牌 · 一键刷机 / 恢复（Windows）
#
# 用法（双击 flash-windows.bat，或让 WorkBuddy 在 PowerShell 里运行）：
#   powershell -ExecutionPolicy Bypass -File flash-windows.ps1            第一次刷：先备份原厂固件，再刷重师大固件
#   powershell -ExecutionPolicy Bypass -File flash-windows.ps1 -Update    以后升级：只换程序，保留 Wi-Fi 和登记信息
#   powershell -ExecutionPolicy Bypass -File flash-windows.ps1 -Check     只检查能不能连上工牌
#   powershell -ExecutionPolicy Bypass -File flash-windows.ps1 -RestoreFactory   换回原厂固件
#   powershell -ExecutionPolicy Bypass -File flash-windows.ps1 -RestoreCqnu      刷回标准重师大固件（自己改坏了用）
#
# 只做：读芯片信息、备份、写入固件。不会执行任何“烧熔丝”（espefuse）类的永久操作。
# 最后一行输出 RESULT=OK 或 RESULT=FAIL。
param([switch]$Update, [switch]$Check, [switch]$RestoreFactory, [switch]$RestoreCqnu, [string]$Port = "")
$ErrorActionPreference = "Continue"
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
Set-Location $Here

function Say($m) { Write-Host "【工牌】$m" }
function Fail($m) { Write-Host ""; Write-Host "【没成功】$m"; Write-Host "RESULT=FAIL"; exit 1 }

$T = Join-Path $Here "tools\esptool-windows-amd64\esptool.exe"
if (-not (Test-Path $T)) { Fail "找不到刷机工具 $T，安装包可能没解压完整（不要直接在压缩包里运行，先解压到桌面）。" }
Get-ChildItem -Recurse $Here | Unblock-File -ErrorAction SilentlyContinue
foreach ($f in "bootloader.bin", "partition-table.bin", "cqnu-badge.bin") {
    if (-not (Test-Path (Join-Path $Here "firmware\$f"))) { Fail "固件文件 firmware\$f 不见了，请重新解压安装包。" }
}

# ---- 找工牌：乐鑫 USB 口的设备号是 VID_303A
if ($Port -eq "") {
    $ports = @(Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
        Where-Object { $_.PNPDeviceID -match "VID_303A" -and $_.Name -match "\((COM\d+)\)" } |
        ForEach-Object { if ($_.Name -match "\((COM\d+)\)") { $Matches[1] } })
    if ($ports.Count -eq 0) { Fail "没找到工牌。请检查：① 用能传数据的 USB-C 线（有的线只能充电）② 线插紧 ③ 按一下工牌侧面的键让它亮屏，再运行一次。" }
    if ($ports.Count -gt 1) { Fail "电脑上插了 $($ports.Count) 张工牌（$($ports -join ', ')），请只插一张。" }
    $Port = $ports[0]
}
Say "找到设备：$Port"

function ET { & $T --chip esp32c3 -p $Port @args 2>&1 | ForEach-Object { "$_" } }

$info = ET flash-id
if ($LASTEXITCODE -ne 0) { $info | Select-Object -Last 5; Fail "连不上工牌。拔掉重插、让工牌亮屏后再试一次。" }
$macLine = $info | Where-Object { $_ -match "^MAC:" } | Select-Object -First 1
$sizeLine = $info | Where-Object { $_ -match "flash size" } | Select-Object -First 1
if (-not $macLine) { $info | Select-Object -Last 8; Fail "读不到工牌编号。" }
$Mac = (($macLine -split "\s+")[1] -replace ":", "").ToUpper()
$Size = ($sizeLine -replace ".*:\s*", "")
Say "工牌编号 $Mac，存储容量 $Size"
$Fw = Join-Path $Here "firmware"
$Bk = Join-Path $Here "backup\factory-$Mac.bin"

if ($RestoreCqnu) {
    Say "正在刷回标准重师大固件…"
    ET write-flash -z 0x0 "$Fw\bootloader.bin" 0x8000 "$Fw\partition-table.bin" 0x10000 "$Fw\cqnu-badge.bin" | Select-Object -Last 3
    if ($LASTEXITCODE -ne 0) { Fail "写入失败，拔掉重插再试。" }
    Say "好了，工牌会自动重启。"; Write-Host "RESULT=OK"; exit 0
}
if ($RestoreFactory) {
    if (-not (Test-Path $Bk)) { Fail "没找到这张工牌（编号 $Mac）的原厂备份 backup\factory-$Mac.bin。" }
    Say "正在恢复原厂固件（约 3 分钟，别拔线）…"
    ET write-flash 0x0 $Bk | Select-Object -Last 3
    if ($LASTEXITCODE -ne 0) { Fail "恢复失败，拔掉重插再试。" }
    Say "恢复好了。"; Write-Host "RESULT=OK"; exit 0
}

if ($Size -notmatch "^8MB") { Fail "这块板子的存储容量是 $Size，不是 8MB，不是我们发的工牌型号，停止刷机。" }
$sec = ET get-security-info
if (($sec -join "`n") -match "(?i)flash encryption:\s*enabled|secure boot:\s*enabled") { Fail "这张工牌开启了加密保护，不能刷第三方固件。请把工牌交给老师。" }
if ($Check) { Say "检查通过：工牌连接正常，可以刷机。"; Write-Host "RESULT=OK"; exit 0 }

New-Item -ItemType Directory -Force -Path (Join-Path $Here "backup") | Out-Null
if (-not $Update) {
    if (Test-Path $Bk) { Say "已经有这张工牌的备份，跳过备份。" }
    else {
        Say "正在备份原厂固件（约 2–4 分钟，别拔线）…"
        ET read-flash 0 0x800000 "$Bk.part" | Select-Object -Last 2
        if ($LASTEXITCODE -ne 0) { Remove-Item "$Bk.part" -ErrorAction SilentlyContinue; Fail "备份没成功，为了安全没有继续刷机。拔掉重插再试一次。" }
        Move-Item "$Bk.part" $Bk
        Say "备份好了：backup\factory-$Mac.bin"
    }
}

if ($Update) {
    Say "正在升级程序（保留 Wi-Fi 和登记信息，约 1 分钟）…"
    ET write-flash -z 0x10000 "$Fw\cqnu-badge.bin" | Select-Object -Last 3
} else {
    Say "正在刷入重师大固件（约 1 分钟，别拔线）…"
    ET write-flash -z 0x0 "$Fw\bootloader.bin" 0x8000 "$Fw\partition-table.bin" 0x10000 "$Fw\cqnu-badge.bin" | Select-Object -Last 3
}
if ($LASTEXITCODE -ne 0) { Fail "写入失败。拔掉重插再运行一次；工牌不会坏。" }
Say "刷好了！工牌会自动重启，屏幕上出现重师大开机画面就成功了。"
Say "接下来：按工牌屏幕提示连 Wi-Fi（2.4G），再扫码登记。"
Write-Host "RESULT=OK"
