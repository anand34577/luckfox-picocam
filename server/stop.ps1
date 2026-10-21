Get-Process mediamtx, edgeai-server -ErrorAction SilentlyContinue | Stop-Process -Force
Write-Host "EdgeAI server stopped."
