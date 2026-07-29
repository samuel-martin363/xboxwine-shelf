@echo off
cd /d "%~dp0"
py xboxwine_uploader.py
if errorlevel 1 pause
