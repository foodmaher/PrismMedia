# PrismTextureStreamerFB 2.3 — Build, Install, and Use

## A. Update the GitHub repository

1. Extract `PrismTextureStreamerFB-2.3.0-adaptive-source.zip`.
2. Open your `PrismTextureStreamerFB-Performance` repository on GitHub.
3. Upload the extracted files and folders into the repository root, preserving
   the folder structure. Allow GitHub to replace files with the same names.
4. Commit the upload to the `main` branch.

The update includes a corrected workflow. It does not call `dumpbin`, so the
previous “dumpbin is not recognized” failure cannot occur.

## B. Build without installing Visual Studio

1. In the GitHub repository, open **Actions**.
2. Select **Build Windows DLL**.
3. Select **Run workflow**, then select the green **Run workflow** button.
4. Wait for the run to turn green. A normal build is usually about 2–5 minutes;
   the first NuGet restore can take a little longer.
5. Open the finished run.
6. Download the `PrismTextureStreamerFB-2.3.0-adaptive` artifact.
7. Extract the downloaded ZIP.

## C. Install

Copy every file and subfolder from the artifact into:

```text
<ETS2 or ATS>\bin\win_x64\plugins
```

Keep these together:

- `PrismTextureStreamerFB.dll`
- `PrismMediaClient.exe`
- the WebView2 support DLLs and `www` folder from the artifact

The Integrated Media Client requires the Microsoft Edge WebView2 Runtime,
which is normally already installed on Windows 10 and Windows 11.

## D. Configure a screen

1. Start ETS2 or ATS.
2. Press **Ctrl+F8**.
3. Add GPS, Dashboard, or Custom.
4. Choose a **Playback Method**:

   - **Native Direct Media** — lowest expected impact; use a local video path
     or a direct media/stream URL.
   - **Integrated Media Client** — recommended for YouTube videos and
     playlists. Paste the YouTube URL and select **Start / Reload**.
   - **Window Capture** — use for any other visible program.

5. Start with the **Balanced** profile: 1280×720 at 30 FPS.
6. Select **Fit** scaling unless you prefer a full-screen centre crop.
7. Select **Apply Unsaved Changes**.

The complete configuration is stored at:

```text
%LOCALAPPDATA%\PrismTextureStreamerFB\config.ini
```

It is restored the next time the game starts.

## E. Media controls and key assignment

The in-game screen panel provides Play/Pause, Previous, Next, Mute, Volume
Down, and Volume Up.

Open **Media Hotkeys**, select a binding, then press the desired key or key
combination. Backspace/Delete clears a binding and Escape cancels. Mark one
media screen as **Use this screen for media hotkeys**. All assignments are
saved in the configuration.

For YouTube playlists, Previous and Next change videos. For Native Direct
Media, they seek backward or forward by 30 seconds.

## F. Read the performance meter

Open **Live Performance Monitor** under a screen. It shows:

- current game FPS;
- estimated FPS lost to measured game-thread plugin work;
- game texture upload time;
- source-worker CPU and GPU-readback time;
- source FPS and dropped frames;
- whether hardware decoding and window-capture bypass are active.

The estimate is most useful for comparing methods and profiles on the same PC.
No moving video path can have literally zero cost: decoding and a texture
update are always required. Native Direct Media removes the window-capture
stage and normally has the lowest cost.

## G. Configure adaptive cabin sound

1. Use **Integrated Media Client** and start the video.
2. Open **Adaptive Cabin Audio**.
3. Enable **head-position adaptive sound**.
4. Start with Spatial strength `0.85`, Speaker direction `0`, Volume facing
   away `0.05`, Outside-cab distance `0.85 m`, Volume when outside `0`, and
   Volume in menus / before driving `50%`.
5. Turn the in-game camera left/right and adjust Speaker direction until the
   sound appears fixed near the truck screen.
6. Open the pause/menu UI and confirm the volume changes to the configured
   menu level.
7. Select an external camera and confirm the menu reports **external camera**
   instead of retaining the last interior head position.
8. Enable **Play media only while truck engine is running**. Stop the engine;
   playback should pause. Start it; playback should resume.

This feature changes only the Integrated Media Client audio session. Native
Direct Media remains unprocessed because it shares the game process audio path.

## H. Adjust screen brightness

Use **Screen Brightness** in the target screen panel:

- `100%` keeps the captured image unchanged and retains the fastest copy path.
- Below `100%` darkens the GPS/dashboard screen.
- Above `100%` brightens it, up to `200%`.

Brightness is saved per screen. Non-default values use a small lookup-table
cost during texture upload.

Set **Edge Colour-Bleed Guard** to `2 px` to prevent full-screen video colours
from tinting the surrounding GPS bezel. Set it to `0` only if a particular
custom screen does not require the guard.

## I. Configure the reverse mirror

1. Press **F2** in ETS2/ATS until the fixed virtual mirror is visible.
2. Open the plugin screen and expand **Automatic Reverse Mirror**.
3. Enable **Show reverse view on this screen**.
4. Enable **Preview / calibrate now**.
5. Adjust Mirror left/top/width/height until the target screen shows only the
   virtual mirror.
6. Disable preview and select reverse gear. The screen switches automatically.

The reverse capture sleeps while driving forward. It uses the game's mirror
render, which already follows the current truck, trailer and articulation.
