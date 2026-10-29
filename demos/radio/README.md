# PS1 DEV CART Radio

Internet radio on a PlayStation 1: a demo of the PS1 DEV CART firmware's USB link. The computer fetches a radio station, and the PlayStation plays it through its own sound chip
(SPU), with the song title on screen.

<p align="center">
  <img src="../../screenshots/ps1/radio.png" alt="PS1 DEV CART Radio playing Smooth Radio, with the song title, the buffer and the SPU RAM used." width="640">
</p>

## What you need

- A PlayStation with a PS1USB cartridge running the PS1 DEV CART firmware, connected to a Windows computer by USB
- Python 3 with the packages of `requirements.txt`:

      pip install -r requirements.txt

## Running it

Run `run.bat`. It opens the radio server in its own window, then sends `radio.ps-exe` to the cartridge and starts the
bridge between the two. Playing starts after about a second of buffering.

With more than one cartridge connected, give the serial number (`ps1transfer -list` shows them):

    run.bat -n A10PVL53

Close both windows to stop.

### Controls

| Button | |
|---|---|
| Left / Right | Previous / next station |
| Up / Down | Volume |

### Other stations and audio files

The server plays six UK stations by default (Smooth, Capital London, Heart, Classic FM, Heart 80s and Radio X Classic
Rock). To play others, give their links or audio files instead, then start the bridge yourself:

    python radio_server.py http://example.com/stream.mp3 song.flac
    ..\..\ps1transfer\ps1transfer -r -e -f radio.ps-exe -net 127.0.0.1:7777

Icecast/SHOUTcast MP3 stations show their song titles. Audio files (MP3, FLAC, WAV, OGG, M4A, ...) and links in other
formats need PyAV (`av` in `requirements.txt`), and show their title tags. At the end of a file the next one plays.

`--rate` sets the sample rate sent to the PlayStation (22050 Hz by default).

## How it works

1. `radio_server.py` decodes the station to mono and encodes it to SPU ADPCM, the PlayStation's own sound format
   (12.6 KB/s at 22050 Hz). It sends the sound at most 2.5 seconds ahead of real time.
2. `ps1transfer -net` is the bridge between the server (TCP) and the cartridge (USB).
3. On the PlayStation, `radio.ps-exe` receives it over the USB link with psxcomb (the PS1 DEV CART's link library),
   keeps up to about 5 seconds in RAM, and feeds a ring of two halves in SPU RAM that one voice plays in a loop. The
   SPU's interrupt address tells it when to refill each half.

The protocol between the server and the PlayStation is in `radio.h`.

## Building

`radio.ps-exe` is included. To build it yourself, with [PSn00bSDK](https://github.com/Lameguy64/PSn00bSDK) set in
`config/Makefile.conf` like the PlayStation tools:

    make

It uses the repo's psxcomb (`../../psxcomb`), screen layout (`devcart_ui.h`) and `psn00b_tool.mk`.
