Tenchu Z - PC recompilation (test build)
=======================================

This is the Xbox 360 game Tenchu Z, recompiled to run natively on Windows.
No game data is included: you need your own copy of the game as a disc image.

Requirements
------------
- Windows 10 or 11, 64-bit, and a DirectX 12 graphics card.
- Your Tenchu Z (Xbox 360) disc image (.iso), about 7 GB. Setup checks that it's the
  version this build was made from before it installs anything.
- About 4.5 GB of free space for the installed game.

Install
-------
1. Run TenchuZ-Setup.exe and choose your disc image (or drop the .iso onto it).
2. Choose where to install (the suggested folder is on the drive with the most free
   space; any folder you can write to works, but not inside Program Files).
   Setup installs the game and copies its data from the disc image into the "game"
   folder there. If it stops part-way, run it again: it continues.
3. Start Tenchu Z from the Start menu or the desktop shortcut.

Once installed, the disc image isn't needed: you can move or delete it.
Saves go to the "user" folder in the install folder. To uninstall, delete the
install folder (back up "user" first to keep your saves) and the shortcuts.
Setup.exe in the install folder re-copies the game data from a disc image if
the "game" folder is damaged.

Controls
--------
The game uses an Xbox controller when one is connected, and keyboard and mouse
otherwise. To use one or the other only, pick it under Esc > Controls > Input.
Press Esc for the game menu: display, controls (input device, change any key, mouse
sensitivity), audio and online settings. The game pauses while it is open, and
settings are saved.

  Move              W A S D        Camera            mouse (or arrow keys)
  Walk (sneak)      C              Attack            left mouse button
  Strafe            right mouse    Aid               Shift
  Jump              Space          Crouch            Ctrl
  Draw sword        F              Item              E
  Reset view        Q / middle     First-person view V
  Map               Tab            Pause             Enter
  Items (d-pad)     1-4, mouse wheel

Multiplayer (System Link over Steam)
-------------------------------------
1. Both players: Steam running and logged in, and friends on Steam. The game runs as
   Steam's test app "Spacewar", so that's what Steam shows you playing.
2. Start the game on both PCs. Each opens a Steam lobby. To get into one lobby, either
   invite your friend from the Steam friends list ("Invite to Lobby") and accept it, or
   just wait a few seconds: a player alone in their lobby joins a friend's.
3. In the game: Mode Select > System Link. One player picks Create Game, the other
   Search for Game.

Options
-------
Most settings are in the Esc menu (saved to tenchuz.toml in the install folder).
A few more can go after tenchuz.exe in the shortcut:
  --tenchuz_netplay=local         System Link between game windows on one PC (testing)
  --log_file=tenchuz.log          write a log (helpful for bug reports)
