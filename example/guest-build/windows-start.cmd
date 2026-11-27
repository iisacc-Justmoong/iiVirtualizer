@echo off
set "IIV_JOB_SCRIPT="
for %%D in (D E F G H I J K L M N O P Q R S T U V W X Y Z) do if exist "%%D:\IIVJOB\stage.cmd" set "IIV_JOB_SCRIPT=%%D:\IIVJOB\stage.cmd"
if not defined IIV_JOB_SCRIPT goto legacy
call "%IIV_JOB_SCRIPT%"
exit /b %errorlevel%
:legacy
C:\iiv\agent\iiv-build-agent.exe C:\iiv\job.json \\.\COM1 >C:\iiv\agent-start.log 2>&1
