@echo off
rem Builds bin\P2Practice.exe (the launcher) and stub\p2route.exe (a stand-in for the real route CLI, for
rem developing and testing the launcher; the real p2route.exe goes next to P2Practice.exe).
rem Uses the C# 5 compiler that ships with .NET Framework 4.8, so there is nothing to install.
setlocal
cd /d "%~dp0"

set "CSC=%SystemRoot%\Microsoft.NET\Framework64\v4.0.30319\csc.exe"
if not exist "%CSC%" set "CSC=%SystemRoot%\Microsoft.NET\Framework\v4.0.30319\csc.exe"
if not exist "%CSC%" (echo Could not find the .NET Framework 4.8 compiler ^(csc.exe^). & exit /b 1)

if not exist bin mkdir bin
"%CSC%" /nologo /target:winexe /platform:anycpu /optimize+ /utf8output /warn:4 ^
  /out:bin\P2Practice.exe /win32icon:src\app.ico /win32manifest:src\app.manifest ^
  /r:System.dll /r:System.Core.dll /r:System.Drawing.dll /r:System.Windows.Forms.dll /r:System.Web.Extensions.dll ^
  src\Program.cs src\Settings.cs src\Maps.cs src\RouteCli.cs src\DemoLibrary.cs src\GameLauncher.cs src\Theme.cs src\Controls.cs src\MainForm.cs
if errorlevel 1 exit /b 1
echo Built bin\P2Practice.exe

if not exist stub mkdir stub
"%CSC%" /nologo /target:exe /platform:anycpu /optimize+ /utf8output /warn:4 /out:stub\p2route.exe stub\p2route_stub.cs
if errorlevel 1 exit /b 1
echo Built stub\p2route.exe (development stand-in; set P2ROUTE to its path to use it)
