# Weather sounds

Recordings from [Freesound](https://freesound.org), all under
[CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/) (public domain).
They were cut and converted to 44.1 kHz 16-bit stereo for the game: the rain
loop is 25 seconds with its end crossfaded into its start, and each thunder
clap starts at its strike, with the silence before it trimmed.

| File | Source |
| --- | --- |
| rain_loop.wav | "heavy rain loop", Freesound, CC0 |
| thunder_1.wav | "thunder close boem", Freesound, CC0 |
| thunder_2.wav | [closeup thunder strike 01](https://freesound.org/s/840628/) by loganzsound, CC0 |
| thunder_3.wav | [thunder 12](https://freesound.org/s/795412/) by lukacafuka, CC0 |

src/audio/weather_sound.cpp plays them: any further thunder_N.wav (N up to 16)
is picked up too.
