"""Build + run tests/live_nts.exe -- hits real providers, not part of CI."""
import subprocess, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts"))
from build import build_mbedtls_archive, MBEDTLS_DIR, find_tool
GCC = find_tool("gcc")

ROOT = Path(__file__).resolve().parent.parent
SRC, BUILD = ROOT/"src", ROOT/"build"
(BUILD/"tests").mkdir(parents=True, exist_ok=True)
arc = build_mbedtls_archive(GCC)
exe = BUILD/"tests"/"live_nts.exe"
cmd = [GCC, "-O2", "-Wall", "-std=c11",
       f"-I{MBEDTLS_DIR/'include'}", f"-I{ROOT/'third_party'}",
       "-DMBEDTLS_CONFIG_FILE=<lunar_mbedtls_config.h>",
       "-o", str(exe),
       str(ROOT/"tests"/"live_nts.c"),
       str(SRC/"nts.c"), str(SRC/"nts_ke.c"), str(SRC/"nts_ef.c"),
       str(SRC/"siv.c"), str(SRC/"clock.c"),
       str(arc), "-lws2_32", "-ladvapi32", "-lbcrypt"]
r = subprocess.run(cmd)
if r.returncode != 0: sys.exit(r.returncode)
sys.exit(subprocess.run([str(exe)]).returncode)
