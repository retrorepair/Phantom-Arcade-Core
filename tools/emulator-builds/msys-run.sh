#!/bin/bash
# msys-run.sh ENV LOGFILE COMMAND...   - run COMMAND inside an MSYS2 environment (ENV = MINGW64 | UCRT64 | MINGW32), log to LOGFILE.
# MSYS2's own bash/make/etc. must come before C:\Windows\System32 (WSL's bash.exe lives there and does not understand C:/ paths).
export MSYSTEM="$1"; shift
LOG="$1"; shift
source /etc/profile
# meson's cmake lookup (Python Path.home()) needs a Windows home directory, which this launcher's environment lacks
export USERPROFILE='C:\Users\joelw'
export HOMEDRIVE='C:'
export HOMEPATH='\Users\joelw'
{
  echo "== $(date)  MSYSTEM=$MSYSTEM  cmd: $*"
  echo "bash: $(command -v bash)"
  "$@"
  echo "EXIT=$?"
} > "$LOG" 2>&1
