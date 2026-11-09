# Movie tests

Tests of the Movie Tools: the STR converter (`gui/strmovie.py`, `gui/movieconv.py`) and the console's movie tester
(`ps1-tools/videotest/videotest.c`). Run them before a release. FFmpeg is needed for some of them, as the Movie Tools'
own download or on the PATH: its STR demuxer and its MDEC and XA decoders are the independent check of the files.

## Without a console

```
python -m unittest tests/movie/test_movie_player.py tests/movie/test_movie_convert.py -v
```

- `test_movie_player.py`: the console's movie tester compiled for the PC (`build_host.py`, `vthost.c`), up to its
  screen drawing. The rest is replaced:
  - the file server reads this PC's files, at a transfer rate the test can set;
  - the MDEC decodes in a time the test can set, and checks each frame's bytes;
  - the SPU plays the sound ring, its interrupt included, into a sound file;
  - the GPU uploads are checked against the VRAM.

  Test movies are played to their end. Each has a beep at the start of each second, so the picture and the sound can
  be measured together. Checked:
  - every frame reaches the MDEC intact, and is shown once, in order, at its pace;
  - the sound played equals FFmpeg's decoding of the file;
  - the picture and the sound stay together, with no drift: NTSC and PAL, RAM and stream modes, 1x and 2x, every
    sound format, after a pause, at the loop;
  - the commands: pause, step, restart, close and open again, colour depth, scale;
  - a slow decoder, a slow file server, frames too big for their sectors;
  - 640-wide movies; 2048-byte, 2352-byte and RIFF CDXA sectors;
  - damaged files: no buffer or VRAM overflow, no hang (`MOVIE_FUZZ=N`: how many damaged copies, default 60).
- `test_movie_convert.py`: the converter.
  - Pictures at every resolution (against the source and FFmpeg's decoding); frames that can't fit their sectors.
  - Each frame at its time, for every speed, frame rate and sound format.
  - The sectors: EDC and ECC (computed here), subheaders, the sound's interleave, frame headers, the end of the file.
  - The XA sound (FFmpeg decodes it); the reader on each sector format and on damaged files.
  - Whole conversions from a test video with a flash and a beep each second:
    - every option checked: frame count, sound length, and the flash and the beep together in the STR;
    - slide shows;
    - a conversion stopped part-way.

## With a console

```
python tests/movie/live_movie_tools.py [--cartridge BILL]
```

Drives the Movie Tools page's "Test on the PS1" with the console connected. Every scenario runs to its end, then a
summary of the checks:
- a RAM movie: pace, pause, next frame, play, restart, 24 bits, picture size, the end, loop
- Stop: the console's movie stopped (read in its memory)
- streamed movies at 2x and 1x
- no USB error
