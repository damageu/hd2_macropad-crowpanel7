---
name: CrowPanel report
about: Bug or hardware note for the Elecrow CrowPanel Advance 7.0 port
title: "[CrowPanel] "
labels: ''
assignees: ''

---

This template is for the CrowPanel Advance 7.0 fork (DIS02170A). The original JC3248 project and its donations stay with Erik Lembke. Do not include a payment request, an email address, or a path from your home directory.

**Firmware**
About-screen version string:

Commit or build you flashed:

App-only update or full erase:

**Link app**
v1.2.6 (first install and tray) or another build:

Was HD2 Macropad Link closed before esptool ran?

COM port (the installer detects the CH340; COM5 is only an example):

Baud used (115200, 230400, or 460800). Do not use 921600:

If the Windows installer failed, attach `install-*.log` from `%LOCALAPPDATA%\HD2MacropadLink\logs\`. Do not paste the full path. It contains your Windows user name. The installer's **Send the log for checking** button opens a short summary and the log folder.

**Board**
PCB revision, if printed on the board:

Connectivity setting (this port expects USB, the serial link):

DIP S1 and S0 (speaker path is S1=0, S0=0):

PH2.0 speaker connected to SPK: yes / no

**What happened**

**What you expected**

**Steps**
1.
2.
3.
