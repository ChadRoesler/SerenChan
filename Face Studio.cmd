@echo off
rem Face Studio: live preview and editor for StackChan sprite faces (Wren, 2 Nov 2026).
rem Double-click to open it in your browser. Close this window to stop it.
cd /d "%~dp0"
for %%V in (3.14 3.11 3.15) do (
  py -%%V -c "import yaml" >nul 2>&1 && ( py -%%V firmware\tools\face_studio.py %* & goto :eof )
)
python -c "import yaml" >nul 2>&1 && ( python firmware\tools\face_studio.py %* & goto :eof )
echo Face Studio needs Python with PyYAML. Try:  py -m pip install pyyaml
pause
