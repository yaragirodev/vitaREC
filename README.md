# vitaREC

vitaREC streams PS Vita / PSTV gameplay from the console to a PC
over Wi-Fi and **records it to a standard AVI file** on the computer. The game
keeps running while the PC is not connected yet — just wait, it will pick up
automatically.

A fork of [VITA2PC](https://github.com/Rinnegatamante/VITA2PC) by Rinnegatamante, reworked for reliable file-based recording:

- "Waiting for connection..." overlay, the game never stops or restarts
- start/stop from the game: **L + Select** menu (stop with Triangle or X)
- a new AVI file per session, auto-reconnect if the PC drops
- fixed async mode: correct real-time pacing, frames are dropped under load
  instead of duplicated (no more sped-up recordings)
- audio capture (8 channels mixed to mono PCM 48 kHz) (EXPERIMENTAL)

## How to use

1. Copy `VITA2PC.suprx`
   to `ur0:tai/`.
2. Add the game to **`ur0:tai/config.txt`**:

   ```
   *YOURGAMECODE
   ur0:tai/VITA2PC.suprx
   ```

   The game code (TITLEID) is the folder name inside `ux0:app/`
   (e.g. `NPXX12345-...`), one block per game. Restart the Vita afterwards.

   > **WARNING:** this plugin works **only inside apps and games**.
   > If you add it under `*ALL` or `*main`, the Vita will freeze on a black
   > screen.

3. On the PC, run the recorder:

   ```
   ./vita-recorder --vita <VITA_IP_ADDRESS> [--out video.avi]
   ```

4. Launch the game — press L + Select, configure settings and press `Start Recording`
5. On PC type your Vita IP address

## Building

### Plugin (`psvita/`)

Requires [VitaSDK](https://vitasdk.org) (installed via vdpm):

```sh
git clone https://github.com/vitasdk/vdpm && cd vdpm
./bootstrap-vitasdk.sh          # wait for it to finish
cd ~/vitasdk
./bin/vdpm install taihen taipool libjpeg-turbo
export VITASDK=$HOME/vitasdk
```

Build:

```sh
cd psvita
mkdir build && cd build
cmake ..
cmake --build .
```

Output: `build/VITA2PC.suprx`

### PC recorder (`pc/`)

```sh
cd pc
make            # Linux / macOS   -> vita-recorder
make windows    # Windows (MinGW) -> vita-recorder.exe
```

## License

GPL-3.0, see [LICENSE.txt](LICENSE.txt). 

Based on
[VITA2PC](https://github.com/Rinnegatamante/VITA2PC) by Rinnegatamante.
