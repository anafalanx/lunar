import sys, time, subprocess, pathlib
sys.path.insert(0, 'scripts')
import ui_iterate as U

U.user32.SetProcessDPIAware()
U.kill_running(); time.sleep(0.3)
subprocess.Popen([str(U.EXE)])
# Immediately: sync should not yet have completed.
time.sleep(0.4)
h = U.find_window()
U.screenshot(h, pathlib.Path('build/ui/ntp-00-cold.png'))
# Give NTP a few seconds to resolve.
time.sleep(5.0)
U.screenshot(h, pathlib.Path('build/ui/ntp-01-synced.png'))
U.user32.PostMessageW(h, 0x0010, 0, 0)
print('ok')
