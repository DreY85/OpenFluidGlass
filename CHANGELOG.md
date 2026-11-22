# Changelog

## 1.0.5

### Update check

- Once a day and at sign-in, Open Fluid Glass asks GitHub for the latest release. When it is newer than yours, Windows shows a notice with a link to the release page. Nothing is downloaded or installed, and GitHub sees only your IP address.
- The settings app has an Updates card on the System tab: Check for updates daily (on by default) and Check now. When a newer release is known, an Update button appears next to More, with Open release page and Skip this version.

### Smoother dragging

- Moving the mouse and dragging windows no longer stutters when the taskbar has its own glass settings and windows use Clarity 0. A screen-sized texture was created and freed almost every frame. It is now kept while Fluid Glass is on. In a 20 second window drag, frames longer than 33 ms went from 7 to 0.

### Glass

- Grain adds a fine static noise over the glass, like frosted or sandblasted glass. Grain in light mode has its own value. Both start at 0, which is off.
- Shadow lift for light mode readability: Readability in light mode brightens only the dark parts of the backdrop, tinted toward the wallpaper around them, and fades out across the curved edge. Bright areas stay clear instead of milky. Off keeps the classic lift.
- Glass tint in light mode sets how far the glass is lifted toward white in light mode. 0 keeps the dark mode color. The default keeps the old look.
- Clear glass for File Explorer in light mode gives File Explorer windows the same glass color as in dark mode, without the milky white layer. The text halo, outline and colors keep the names readable. Other apps and Start follow Glass tint in light mode. It is off by default.
- Clear empty space below Explorer items shows clear glass in the empty part of the file view in light mode. With Shadow lift on, the last visible row fades into it. The navigation pane and the status bar keep their material.

### Text

- File Explorer text style has two new looks. Engraved adds a single edge, under dark text in light mode and above light text in dark mode. Etched adds a fine upper shadow and a lower reflection and keeps your text color. Standard uses the halo, glow and outline settings as before.
- Title bars that Windows draws for other apps, such as classic Notepad, Regedit and Control Panel, take the File Explorer text color of their mode while the window is active. Inactive titles stay dimmed. Title bar text style picks Standard, Engraved or Etched. Apps that draw their own title bars are not affected. The color card is now called Text colors and title bars.
- In light mode, the column headers in File Explorer (Name, Date modified and the rest) get the text color, halo and outline too.
- The outline and glow of a row scrolled under the column headers no longer show through them as a dark ghost.

### Taskbar

- Use separate blur and readability gives the floating taskbar its own blur and readability for light and dark mode.
- Tray text style styles the clock, the date and the system icon glyphs on the floating taskbar.
- The taskbar text effects reuse cached glyph masks.

## 1.0.4

### File Explorer colors

- You can pick your own colors for File Explorer text, separately for Light and Dark mode. There are three colors per mode: text (file and folder names, the header and the command bar), details (descriptions, sizes and dates), and icons (the arrows, refresh, search and the other command bar glyphs). Colorful command bar icons and file icons keep their look.
- Each mode has a palette list with ten matched sets. Choosing one fills in all three colors at once.
- Use System Colors puts every color back to the Windows default. Hover, pressed and disabled buttons keep their usual look.

### Text halo in light mode

- In light mode, a soft white halo sits behind the dark and grey text in File Explorer, so names, sizes and dates stay readable over any wallpaper. It covers the file list, the navigation pane and the header with its tabs, address bar, search box and command bar. It is on by default.
- The taskbar tray text, its glyph icons and the clock get the same halo while the floating pills show.
- Text halo in light mode sets how strong it is, and 0 turns it off. Halo size in light mode sets how far it reaches, from a tight outline at 3 pixels up to a wide soft glow like the Windows 7 title bars.
- After a switch from light to dark mode, the tray text and icons turn white again. They used to stay black with a faint halo.

### White flashes

- In light mode, a folder window no longer shows a white sheet for a moment when you restore it from the taskbar or open a new one. The blank surface Windows gives the window is painted as clear glass until File Explorer draws over it.
- Taskbar thumbnails and Alt+Tab no longer show File Explorer as a white sheet in light mode. Readability in light mode now lifts the glass only where something is behind it.

### Glass

- Softness 0 costs less. With Clarity above 0, the glass now reads the background once per pixel instead of 13 times. The picture is the same.
- With Direct3D rendering off, the blur no longer creates its bitmap again on every frame.

### Text glow and outline

- In dark mode, File Explorer text can glow softly in its own color, like neon. Text glow in dark mode sets how bright it is and Glow size how far it reaches. It works with the colors you pick too. It is off by default, and disabled buttons never glow.
- In dark mode, File Explorer text can also get a thin, crisp black outline, like a stroke in a photo editor, so light text stays readable over bright backgrounds. Text outline in dark mode sets how dark it is and Outline width picks 1 or 2 pixels. It is off by default.
- In light mode, File Explorer text can get a thin white outline too. Text outline in light mode sets how strong it is and Outline width in light mode picks 1 or 2 pixels. It works with the halo or on its own. It is off by default.
- The halo, glow and outline no longer stop in a straight line at the left of the text on This PC and in folders. They now follow the first letter, as they already did in the navigation pane.
- The glow, the outline and the light mode halo are drawn together in one pass and only around the letters, so scrolling large folders stays smooth with them on.

### App

- Card titles are bold.
- A tab page keeps its scroll position when the window loses or regains the focus. It used to jump back to the top.
- Readability in light mode, Text halo and Halo size are locked while Windows uses dark mode for apps, since they only change light mode. The glow and outline settings are locked in light mode the same way.

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
