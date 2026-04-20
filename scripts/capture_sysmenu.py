import sys, time, subprocess, pathlib, ctypes, ctypes.wintypes as wt
sys.path.insert(0, 'scripts')
import ui_iterate as U
from PIL import ImageGrab

U.user32.SetProcessDPIAware()
U.kill_running(); time.sleep(0.3)
subprocess.Popen([str(U.EXE)]); time.sleep(0.8)
h = U.find_window()
u = ctypes.windll.user32
u.SetForegroundWindow(h); time.sleep(0.2)

# Open the system menu at the window's top-left (where the title-bar icon is).
# Use WM_SYSCOMMAND SC_KEYMENU with ' ' (space) == open system menu.
# Actually simpler: send WM_SYSCOMMAND with SC_MOUSEMENU = 0xF090 at icon pos.
r = wt.RECT(); u.GetWindowRect(h, ctypes.byref(r))
# Title-bar icon sits near top-left inside the non-client area.
icon_x = r.left + 12; icon_y = r.top + 18
u.SetCursorPos(icon_x, icon_y); time.sleep(0.1)
u.mouse_event(0x0002, 0, 0, None, None); time.sleep(0.05)
u.mouse_event(0x0004, 0, 0, None, None); time.sleep(0.6)

# Screenshot a generous region below/right of the title bar.
box = (max(0, r.left - 10), max(0, r.top - 10), r.right + 400, r.bottom + 400)
img = ImageGrab.grab(bbox=box)
out = pathlib.Path('build/ui/crit-menu-02.png')
out.parent.mkdir(parents=True, exist_ok=True)
img.save(out)
print('saved', out, img.size)

# Close menu + app.
u.keybd_event(0x1B, 0, 0, 0); u.keybd_event(0x1B, 0, 2, 0); time.sleep(0.2)
u.PostMessageW(h, 0x0010, 0, 0)
