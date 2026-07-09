' Launches the Claude Buddy companion invisibly (used from shell:startup).
Set sh = CreateObject("WScript.Shell")
dir = "C:\Users\rdenoyer\code\applications\Desktop Buddy\companion"
sh.CurrentDirectory = dir
sh.Run "cmd /c python -u buddy_companion.py > """ & dir & "\buddy.log"" 2>&1", 0, False
