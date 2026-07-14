' Launches the Desktop Buddy companion invisibly (used from shell:startup).
Set sh = CreateObject("WScript.Shell")
dir = "C:\Users\rdenoyer\code\applications\Desktop Buddy\companion"
py = "C:\Users\rdenoyer\code\applications\Desktop Buddy\.venv\Scripts\python.exe"
sh.CurrentDirectory = dir
sh.Run "cmd /c """"" & py & """ -u buddy_companion.py > """ & dir & "\buddy.log"" 2>&1""", 0, False
