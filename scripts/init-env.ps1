# ESP-IDF 开发环境自动配置脚本

# 设置Python环境编码
Write-Host "配置Python环境编码..." -ForegroundColor Green
Set-Item env:PYTHONIOENCODING utf-8
Set-Item env:PYTHONUTF8 1

# 设置控制台编码
Write-Host "配置控制台编码..." -ForegroundColor Green
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
[Console]::InputEncoding = [System.Text.Encoding]::UTF8

# 检查并设置IDF_PYTHON_ENV_PATH
$expectedPythonEnv = "C:\Espressif\python_env\idf5.3_py3.12_env"
if ($env:IDF_PYTHON_ENV_PATH -ne $expectedPythonEnv) {
    Write-Host "设置IDF_PYTHON_ENV_PATH..." -ForegroundColor Green
    [Environment]::SetEnvironmentVariable("IDF_PYTHON_ENV_PATH", $expectedPythonEnv, "User")
    $env:IDF_PYTHON_ENV_PATH = $expectedPythonEnv
}

# 检查ESP-IDF环境变量
$idfPath = "C:\Espressif\frameworks\esp-idf-v5.3.2"
if (Test-Path $idfPath) {
    Write-Host "重新导出ESP-IDF环境变量..." -ForegroundColor Green
    & "$idfPath\export.ps1"
} else {
    Write-Host "错误: 未找到ESP-IDF路径 $idfPath" -ForegroundColor Red
    exit 1
}

# 验证环境配置
Write-Host "
验证环境配置:" -ForegroundColor Cyan
Write-Host "Python I/O编码: $env:PYTHONIOENCODING"
Write-Host "Python UTF8: $env:PYTHONUTF8"
Write-Host "IDF Python环境: $env:IDF_PYTHON_ENV_PATH"

Write-Host "
✅ ESP-IDF开发环境配置完成" -ForegroundColor Green