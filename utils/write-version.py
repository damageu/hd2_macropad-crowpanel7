import os
import sys
import subprocess

versionPath = "src/version.h"

# PlatformIO sets PIOENV. Import("env") is only defined when PlatformIO
# loads this file as an extra script, not when CI runs it as plain Python.
pioenv = os.environ.get("PIOENV", "")
if not pioenv:
    try:
        Import("env")
        pioenv = env.get("PIOENV", "")
    except Exception:
        pioenv = ""

forced = os.environ.get("HD2MP_VERSION")
if not forced and pioenv == "crowpanel-adv7":
    # Rebase of the tested cp7.2 port onto upstream v1.9.1 plus the two
    # commits after that tag. This is not the hardware-tested v1.8.6-cp7.2 binary.
    # Set HD2MP_VERSION to override. This path does not run `git pull`.
    forced = "v1.9.1-cp7.2"

if forced:
    versionNumber = forced
elif len(sys.argv) > 1 and sys.argv[1][0] == 'v':
    versionNumber = sys.argv[1]
else:
    result = subprocess.run(['git', 'pull'])
    result = subprocess.run(['git', 'describe', '--abbrev=0', '--tags'], stdout=subprocess.PIPE)
    versionNumber = result.stdout.decode('utf-8').replace("\n", "")

with open(versionPath, "w") as file:
    file.write('#define SW_VER "' + versionNumber + '"\n')
