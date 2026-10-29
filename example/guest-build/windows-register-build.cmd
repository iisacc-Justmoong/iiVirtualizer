@echo off
schtasks /Create /TN iiVirtualizerBuild /SC ONSTART /RU SYSTEM /RL HIGHEST /TR C:\iiv\start.cmd /F >C:\iiv\task-start.log 2>&1
if errorlevel 1 exit /b 1
schtasks /Run /TN iiVirtualizerBuild >>C:\iiv\task-start.log 2>&1
