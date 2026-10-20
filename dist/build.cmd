@echo off
REM build.cmd -- package relay-bridge-demo into dist\ and zip it (Windows cmd).
REM Usage:  build.cmd      (requires Python on PATH; optional: gcc or cl for the client binary)
setlocal
cd /d "%~dp0"

echo == relay-bridge-demo : build/package ==
if exist dist rmdir /s /q dist
mkdir dist\client dist\server

copy server\fake_server.py dist\server\ >nul
echo   [1/4] server\fake_server.py  -^> dist\server\

copy client\fake_webui.c dist\client\ >nul
copy build.sh dist\ >nul
echo   [2/4] client sources -^> dist\client\

set BIN=
where gcc >nul 2>nul
if %errorlevel%==0 (
  echo   [3/4] gcc found: compiling client...
  gcc -O2 -o dist\client\fake_webui.exe client\fake_webui.c -lws2_32
  if not errorlevel 1 (
    set BIN=client\fake_webui.exe
    echo         compiled: dist\client\fake_webui.exe
  ) else (
    echo         gcc failed -- shipping source only
  )
) else (
  echo   [3/4] no C compiler found -- shipping source + build instructions
  echo         build with:  gcc -O2 -o fake_webui.exe fake_webui.c -lws2_32
)

copy README.md dist\ >nul
echo   [4/4] README.md -^> dist\

python -c "import zipfile,os; z=zipfile.ZipFile('dist\\relay-bridge-demo.zip','w',zipfile.ZIP_DEFLATED); [z.write(os.path.join(r,f), os.path.relpath(os.path.join(r,f),'dist')) for r,_,fs in os.walk('dist') for f in fs]; z.close(); print('   wrote dist\\relay-bridge-demo.zip')"
echo == done ==
dir dist
endlocal