# Changelog

## 1.0.4

### File Explorer colors

- You can pick your own colors for File Explorer text, separately for Light and Dark mode. There are three colors per mode: text (file and folder names, the header and the command bar), details (descriptions, sizes and dates), and icons (the arrows, refresh, search and the other command bar glyphs). Colorful command bar icons and file icons keep their look.
- Each mode has a palette list with ten matched sets. Choosing one fills in all three colors at once.
- Use System Colors puts every color back to the Windows default. Hover, pressed and disabled buttons keep their usual look.

### Text glow

- In dark mode, File Explorer text can glow softly in its own color, like neon. Text glow in dark mode sets how bright it is and Glow size how far it reaches. It works with the colors you pick too. It is off by default, and disabled buttons never glow.

### App

- Card titles are bold.
- Readability in light mode, Text halo and Halo size are locked while Windows uses dark mode for apps, since they only change light mode. The two glow settings are locked in light mode the same way.

## 1.0.3

### Windows build 26100.9974

- The glass works again after the Windows update that brings DWM to build 26100.9974. Before, DWM crashed every few seconds once the new debug symbols were in place, and the screen kept flickering. That update changed a few instructions OpenGlass patches inside DWM. The patch now accepts both the old and the new form, and older Windows builds get exactly the same patch as before.
- If a future update changes DWM in a way OpenGlass does not recognise, that one part now switches itself off instead of taking DWM down.

### Crash guard

- If DWM crashes twice in a row right after the glass loads, on the same DWM version, the service stops loading the glass. Windows keeps running with its normal look. The guard resets by itself after a Windows update that changes DWM, or once the glass runs for a minute without a crash. To clear it by hand, delete the registry key `HKLM\SOFTWARE\OpenGlass\CrashGuard`.

### Taskbar

- Light rectangles with cyan edges no longer stay over the tray and clock pills of the floating taskbar. They appeared after a hover over the tray icons, and on the corners of the clock when the minute changed. Small redraws over a pill now keep the glass safety zone, so the glass no longer picks up an old frame.

## 1.0.2

### Updates and uninstall

- Updating no longer asks for a Windows restart. File Explorer keeps two folder extensions loaded, so setup now moves the loaded copies aside and puts the new ones in place. The Explorer restart offered at the end loads the new ones.
- Uninstalling no longer asks for a Windows restart either. A new option on the uninstall page, ticked by default, restarts File Explorer so the glass goes away at once.
- The empty install folder is removed after an uninstall.

### Light and dark mode

- The app follows the Windows light and dark mode live. Titles no longer stay dark on a dark window after Windows switches to Light.
- The taskbar and Start glass follow the Windows mode, the same one their text follows. With Windows in light mode and apps in dark mode, their glass is no longer darkened behind black text.
- Light mode readability now lifts only the brightness and keeps the color. A dark wallpaper no longer turns into a milky gray veil, and mid tones keep their contrast.
- Light mode readability starts at 0.80, the same value the app resets to. The installer used to write 0.40.

### Glass

- At a low highlight focus, two corners no longer show a hard diagonal line where the rim light turns away from the light.

### App

- The app icon shows the refraction through the glass rim more clearly.

## 1.0.1

### Faster glass

- The refraction shader does less work for the same picture. Past the bezel nothing bends, so the optics now run only inside it. And where red, green and blue bend alike, the backdrop is sampled once instead of three times. The output is identical pixel for pixel. Measured on a GTX 980M, each of the two changes made the glass pass 15 to 33% faster on its own.
- The taskbar and Start glass wake up less often when nothing changes.

### Start and Search

- Start's glass stays when you right-click the taskbar while Start is open, and when you pick an item from that menu.
- The glass leaves together with Start, whether you close it with the Win key, Esc, the Start button or a click outside.
- Hover and pressed fills in Start are stronger while the glass shows, and a tile hover no longer flashes grey.

### Taskbar

- Hover, pressed and running app fills on the pills are stronger while the glass shows.
- A tiny hidden window that Explorer sometimes leaves under the taskbar no longer makes the pills redraw all the time.
- Tooltips with dark text keep their own look.

### File Explorer

- A folder window is glass from its first frame. It no longer flashes dark the first time you open one after a reboot.

### App and installer

- New app icon.
- The blur mix slider is now called Clarity, the name of the setting it changes.
- The installer is in English only for now. The other languages were never translated for Fluid Glass.

### Building

- `OpenGlassTaskbar\build.cmd` finds Visual Studio through `vswhere` instead of a fixed folder.

## 1.0.0

First public release.
