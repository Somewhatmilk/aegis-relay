@echo off
REM build.cmd -- package aegis-relay into dist\ and zip it (Windows cmd).
REM Usage:  build.cmd      (requires Python on PATH; optional: gcc or cl for the client binary)
setlocal
cd /d "%~dp0"

echo == aegis-relay : build/package ==
if exist dist rmdir /s /q dist
mkdir dist\client dist\server

copy server\relay_server.py dist\server\ >nul
echo   [1/4] server\relay_server.py  -^> dist\server\

copy client\client.c dist\client\ >nul
copy build.sh dist\ >nul
echo   [2/4] client sources -^> dist\client\

set BIN=
set LOG=%TEMP%\aegis-client-build.log
where gcc >nul 2>nul
if %errorlevel%==0 (
  echo   [3/4] gcc found: compiling client...
  gcc -O2 -o dist\client\client.exe client\client.c -lws2_32 2>%LOG%
  if not errorlevel 1 (
    set BIN=client\client.exe
    echo         compiled: dist\client\client.exe
  ) else (
    echo         gcc failed (see %LOG%) -- shipping source only
  )
) else (
  echo   [3/4] no C compiler found -- shipping source + build instructions
  echo         build with:  gcc -O2 -o client.exe client.c -lws2_32
)

copy README.md dist\ >nul
echo   [4/4] README.md -^> dist\

set PY=python
where python >nul 2>nul || set PY=python3
where %PY% >nul 2>nul || set PY=py
where %PY% >nul 2>nul || (
  echo   error: no python/python3/py on PATH
  exit /b 1
)

%PY% -c "import zipfile,os; z=zipfile.ZipFile('dist\\aegis-relay.zip','w',zipfile.ZIP_DEFLATED); [z.write(os.path.join(r,f), os.path.relpath(os.path.join(r,f),'dist')) for r,_,fs in os.walk('dist') for f in fs]; z.close(); print('   wrote dist\\aegis-relay.zip')"
echo == done ==
dir dist
endlocal