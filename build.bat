@echo off
rem เปิด "x64 Native Tools Command Prompt for VS" แล้วรันไฟล์นี้
rem วาง ScriptHookV SDK ไว้ที่ sdk\inc (natives.h types.h enums.h main.h) และ sdk\lib\ScriptHookV.lib
cl /nologo /LD /O2 /EHsc /std:c++17 /Isdk\inc main.cpp sdk\lib\ScriptHookV.lib user32.lib /Fe:NpcWar.asi
rem ถ้า SDK ของคุณใช้ชื่อ TASK / MISC แทน AI / GAMEPLAY ให้เติม /DSHV_NEW_NAMESPACES
