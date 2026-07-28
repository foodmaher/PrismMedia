# PrismTextureStreamerFB 2.0 — Build, Install, and Use

## A. Update the GitHub repository

1. Extract `PrismTextureStreamerFB-2.0.0-GitHub-update.zip`.
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
6. Download the `PrismTextureStreamerFB-2.0.0-performance` artifact.
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
