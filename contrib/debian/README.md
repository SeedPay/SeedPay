
Debian
====================
This directory contains files used to package Seedpayd/Seedpay-qt
for Debian-based Linux systems. If you compile Seedpayd/Seedpay-qt yourself, there are some useful files here.

## Seedpay: URI support ##


Seedpay-qt.desktop  (Gnome / Open Desktop)
To install:

	sudo desktop-file-install Seedpay-qt.desktop
	sudo update-desktop-database

If you build yourself, you will either need to modify the paths in
the .desktop file or copy or symlink your Seedpayqt binary to `/usr/bin`
and the `../../share/pixmaps/Seedpay128.png` to `/usr/share/pixmaps`

Seedpay-qt.protocol (KDE)

