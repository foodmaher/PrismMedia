# PrismMedia 4.0.0

PrismMedia puts independent video or captured-window content on ETS2/ATS truck displays. Version 4.0.0 uses a private accessory adapter for add-on tablets and GPS units instead of identifying a screen by draw order.

## Install

Copy these items to `<game>\bin\win_x64\plugins`:

- `PrismMedia.dll`
- the complete `PrismMedia` folder

Open the interface with **Ctrl+F8**.

## Bind media to an installed accessory

1. Add a **Custom** display in PrismMedia and choose its media.
2. Open **System** and select **Scan installed truck accessories**.
3. Choose one display from the current truck and select **Create/update local adapter mod**.
4. Open Mod Manager and enable **PrismMedia Generated Display** above the original accessory mod.
5. Restart the game once.

After changing or buying an accessory, leave the upgrade shop, make a quicksave if needed, then scan again. The save must be plain text; set `g_save_format` to `2` and save once if Prism reports an encoded save.

Prism never edits or republishes the source accessory mod. It creates this local structure:

```text
Documents\Euro Truck Simulator 2\mod\PrismMedia_Generated_Display\
├── manifest.sii
├── description.txt
├── def\...\selected_accessory.sii
├── vehicle\prismmedia\<display-id>\model.pmd
├── vehicle\prismmedia\<display-id>\model.pmg
└── automat\pm\<private-material>.mat
```

The selected accessory definition keeps the game's native UI target, while its copied 3D model uses a private material that points to the display's Prism texture. Other GPS screens therefore keep their native routes and cannot be made black by a shared global replacement.

## Build

Open `PrismTextureStreamerFB.sln` in Visual Studio 2022 or run:

```powershell
.\build-release.ps1
```

The product version is defined once in `PrismTextureStreamerFB\version.h`. GitHub Actions and the local package script read that value for every future run and artifact. The runtime version remains **4.0.0**.

Production packages contain the plugin, media client, recommended configuration and optional SPF camera bridge. Diagnostic consoles, scripts, frame probes and PDB files are not installed; symbols are uploaded as a separate short-lived Actions artifact.

