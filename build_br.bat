@echo off
set MSYSTEM=
set BR_HW=_hw1
set BR_SYS=_playstation
call C:\Users\rpf16\esp-idf\export.bat >NUL 2>&1
idf.py build
