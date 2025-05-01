# PowerShell配置文件
# 自动运行ESP-IDF环境初始化脚本

$scriptPath = "d:\dev\dudu-esp32\scripts"
$initEnvScript = Join-Path $scriptPath "init-env.ps1"

if (Test-Path $initEnvScript) {
    Write-Host "正在初始化ESP-IDF开发环境..." -ForegroundColor Yellow
    . $initEnvScript
} else {
    Write-Host "警告: 未找到ESP-IDF环境初始化脚本" -ForegroundColor Red
}