# PrismMedia 4.0.0

PrismMedia puts independent video or captured-window content on ETS2/ATS truck displays. Version 4.0.0 uses a private accessory adapter for add-on tablets and GPS units instead of identifying a screen by draw order.

## Install

Copy these items to `<game>\bin\win_x64\plugins`:

- `PrismMedia.dll`
- the complete `PrismMedia` folder

Open the interface with **Ctrl+F8**.

## Automatic TJS accessory bridge

On the first startup PrismMedia checks both locations automatically:

- `Documents\Euro Truck Simulator 2\mod` (or the ATS equivalent)
- every Steam library's Workshop folder for ETS2/ATS

The current `game.log.txt` load record is used to prefer the TJS package that
the game actually loaded. Prism groups all compatible definitions by their 3D
model and drawable texture, then creates stable Custom display entries and a
local adapter mod. No save conversion, `g_save_format`, scan button or
RenderDoc capture is needed.

When PrismMedia reports that generation is complete:

1. Open Mod Manager.
2. Enable **PrismMedia Generated Display** above the original TJS mod.
3. Restart the game once.
4. Open PrismMedia with **Ctrl+F8**, enable the generated Custom display that
   matches the installed accessory, and choose its media.

The bridge is regenerated only when the source package changes. Buying or
changing an accessory does not require another scan because all compatible
definitions are already covered.

Prism never edits or republishes the source accessory mod. It creates this local structure:

```text
Documents\Euro Truck Simulator 2\mod\PrismMedia_Generated_Display\
├── manifest.sii
├── description.txt
├── def\...\compatible_accessories.sii
├── vehicle\prismmedia\<stable-id>\model.pmd
├── vehicle\prismmedia\<stable-id>\model.pmg
└── automat\pm\<private-material>.mat
```

Each copied model uses its own private material and Prism texture. Other GPS
screens keep their native routes and cannot be made black by a shared global
replacement.

## Build

Open `PrismTextureStreamerFB.sln` in Visual Studio 2022 or run:

```powershell
.\build-release.ps1
```

The product version is defined once in `PrismTextureStreamerFB\version.h`. GitHub Actions and the local package script read that value for every future run and artifact. The runtime version remains **4.0.0**.

Production packages contain the plugin, media client, recommended configuration and optional SPF camera bridge. Diagnostic consoles, scripts, frame probes and PDB files are not installed; symbols are uploaded as a separate short-lived Actions artifact.

