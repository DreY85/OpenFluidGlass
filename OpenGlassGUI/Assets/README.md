# Open Fluid Glass icon

The application icon is the **Membrana** artwork made for Open Fluid Glass: a clear glass tile with a glowing pill, whose end bends where it meets the tile's thick rim, the way the app's glass refracts along its edges.

## Files

- [`OpenGlass.png`](OpenGlass.png) is the 256×256 RGBA artwork. Its pixels match the ICO's 256 pixel image.
- [`OpenGlass.ico`](OpenGlass.ico) is the application icon. It contains 16, 20, 24, 32, 40, 48, 64, 96, 128, and 256 pixel images, all stored as uncompressed 32-bit DIBs with alpha transparency.

The ICO is embedded in `OpenGlassGUI.exe` (main window, Start menu, installed apps list) and `pillhost.exe`, and the installer uses it as its own icon.

## Previous icon

The earlier icon was adapted from **Window Apps (48 Filled)** in Microsoft's [Fluent UI System Icons](https://github.com/microsoft/fluentui-system-icons), Copyright (c) 2020 Microsoft Corporation, under the [MIT License](FluentIcons.LICENSE.txt). Its sources, [`window-apps-filled.original.svg`](window-apps-filled.original.svg) and [`OpenGlass.svg`](OpenGlass.svg), are kept here, and the license is still embedded in `OpenGlassGUI.exe` as the `IDR_FLUENT_ICONS_LICENSE` resource.
