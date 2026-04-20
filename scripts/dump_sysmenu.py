import sys, time, subprocess, ctypes, ctypes.wintypes as wt, pathlib
sys.path.insert(0, 'scripts')
import ui_iterate as U

U.user32.SetProcessDPIAware()
U.kill_running(); time.sleep(0.3)
subprocess.Popen([str(U.EXE)]); time.sleep(0.8)
h = U.find_window()
u = ctypes.windll.user32
u.GetSystemMenu.restype = wt.HMENU
m = u.GetSystemMenu(h, False)
n = u.GetMenuItemCount(m)
MF_BYPOSITION = 0x400
MF_SEPARATOR = 0x800
print(f'items: {n}')
for i in range(n):
    buf = ctypes.create_unicode_buffer(128)
    u.GetMenuStringW(m, i, buf, 128, MF_BYPOSITION)
    state = u.GetMenuState(m, i, MF_BYPOSITION)
    cid = u.GetMenuItemID(m, i)
    label = '<SEP>' if state & MF_SEPARATOR else buf.value
    print(f'  [{i}] id=0x{cid:08x} state=0x{state:04x} label={label!r}')
u.PostMessageW(h, 0x0010, 0, 0)
