Remote Car Control Project
==========================

Did this for university, it starts in place of init and allows you to control a remote car. It depends on a slightly patched version of microwindows-0.92 (which I haven't published) and hci.c/bluetooth.c from BlueZ. It's a good modern example of using standalone Bluetooth on Linux as well as using the MicroWindows UI framework.

Decided to publish it because it's a good example of how to use Bluetooth (including PIN pairing) on Linux without needing to run any of the BlueZ daemons (which in turn depend on Systemd and Dbus), which leads to less bloated images and fast boot time. This code is a mess and some parts of it are based off other GPL licensed code so I'm releasing it under GPL.

It's developed to run on Raspberry Pi but it would work on pretty much anything that runs Linux and has HCI based bluetooth compiled into the kernel. By default, all debug loging goes to ttyAMA0 which is the serial port on the Raspberry Pi. The HID/event code is terrible because the documentation was confusing so I just hacked around it, I'm fully aware of how bad it actually is. 
