@echo off
REM Launcher for a packaged BeamCommander3 Windows release (built by
REM .github/workflows/release.yml). laser_daemon.exe serves the bundled
REM frontend_dist itself - just run this and it opens your browser.
cd /d "%~dp0"
start "" "laser_daemon.exe"
timeout /t 2 /nobreak >nul
start "" "http://localhost:8000"
