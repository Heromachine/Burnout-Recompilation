Burnout Recompilation 0.1.0 for Windows
=======================================

A native PC build of Burnout (Xbox, Criterion Games / Acclaim, 2002),
recompiled from your own copy of the game. No game data is included: the
first time you start it, it asks for your Burnout (USA) disc image and
builds the game from it, on your computer.

What you need
-------------
- Windows 10 or 11, 64-bit
- A graphics card with Direct3D 11
- Your own Burnout (USA) Xbox disc image (.iso)
- About 6 GB of free disk space for the one-time build
- An Xbox-style controller is recommended

Starting
--------
Keep the whole folder together (Burnout.exe needs the tools folder
beside it). It runs from anywhere, but the one-time build is faster from
a local disk than from a network share.

1. Run Burnout.exe.
   (Windows may warn that it is from an unknown publisher: choose
   "More info", then "Run anyway".)
2. First time only: choose your disc image and press Continue. The game
   is built from it. This takes a few minutes and happens once.
3. Press Play.

Later launches go straight to the menu. Updating to a new release
rebuilds the game once, the same way.

Options
-------
- Brightness: Original, in the middle, is the game's own picture. Move
  the slider left for a darker picture, right to lighten the dark parts.
- Resolution: 1x is the Xbox's own 640 x 480, 2x (the default) is
  1280 x 960, 3x is 1920 x 1440. Higher is sharper and needs a faster
  graphics card.
- Run the graphics chip on its own thread: on by default, and much
  faster. Turn it off only if the game hangs or draws wrongly.
- Full screen: fills the screen, with black bars at the sides on a
  widescreen monitor so the picture keeps its shape. F11 or Alt+Enter
  switches at any time, in the game as well.
- Saved games folder: where your saves are kept. Choosing a new folder
  does not move saves already made; copy them across yourself. "Open
  folder" shows the current one.

On a laptop with two graphics chips, the launcher asks Windows for the
faster one.

Where things are kept
---------------------
%LOCALAPPDATA%\BurnoutRecomp\
    settings.ini   the disc image you chose, and your Options
    work\          the extracted game and the build (delete it to rebuild)
    logs\          launcher, build and game logs; please include the
                   newest ones with any bug report
    saves\         saved games, unless you choose another folder in
                   Options

Close the game window to quit (Alt+F4 in full screen).

Controls in the menus: mouse, keyboard, or an Xbox-style controller
(D-pad or left stick to move, A to choose, B to go back).
