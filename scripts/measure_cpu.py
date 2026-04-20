import subprocess, time, sys, pathlib, ctypes
import ctypes.wintypes as wt
sys.path.insert(0, 'scripts')
import ui_iterate as U

U.user32.SetProcessDPIAware()
U.kill_running(); time.sleep(0.3)
p = subprocess.Popen([str(U.EXE)])
time.sleep(4.0)
h = U.find_window()
U.screenshot(h, pathlib.Path('build/ui/opt2-verify.png'))

PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
k32 = ctypes.windll.kernel32
psapi = ctypes.windll.psapi
hproc = k32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, p.pid)

def ptimes():
    ft = (wt.FILETIME*4)()
    k32.GetProcessTimes(hproc, *(ctypes.byref(x) for x in ft))
    def to100ns(f): return (f.dwHighDateTime<<32)|f.dwLowDateTime
    return to100ns(ft[2]) + to100ns(ft[3])

class PMC(ctypes.Structure):
    _fields_ = [('cb', wt.DWORD),('PageFaultCount', wt.DWORD),
        ('PeakWorkingSetSize', ctypes.c_size_t),('WorkingSetSize', ctypes.c_size_t),
        ('QuotaPeakPagedPoolUsage', ctypes.c_size_t),('QuotaPagedPoolUsage', ctypes.c_size_t),
        ('QuotaPeakNonPagedPoolUsage', ctypes.c_size_t),('QuotaNonPagedPoolUsage', ctypes.c_size_t),
        ('PagefileUsage', ctypes.c_size_t),('PeakPagefileUsage', ctypes.c_size_t),
        ('PrivateUsage', ctypes.c_size_t)]
def mem():
    m = PMC(); m.cb = ctypes.sizeof(m)
    psapi.GetProcessMemoryInfo(hproc, ctypes.byref(m), m.cb)
    return m.WorkingSetSize, m.PrivateUsage

t0 = ptimes(); w0 = time.perf_counter()
time.sleep(5.0)
t1 = ptimes(); w1 = time.perf_counter()
cpu = (t1-t0)/1e7; wall = w1-w0
ws, priv = mem()
print(f'CPU: {100*cpu/wall:.2f}% of one core')
print(f'Working set: {ws/1024/1024:.1f} MB')
print(f'Private:     {priv/1024/1024:.1f} MB')
k32.CloseHandle(hproc)
U.user32.PostMessageW(h, 0x0010, 0, 0)
