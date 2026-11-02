; Languages kept aside for a later release. Not included in the build: the installer is English only for now,
; like the app. To add one, move its line under [Languages] in OpenGlass.iss and translate every entry of
; [CustomMessages] there for it (german.StartTaskbar=..., and so on), so the setup never mixes languages.
; Romanian is planned too. Inno Setup does not ship Romanian.isl, so it needs the unofficial translation file.

[Languages]
Name: "german"; MessagesFile: "compiler:Languages\German.isl"
Name: "french"; MessagesFile: "compiler:Languages\French.isl"
Name: "spanish"; MessagesFile: "compiler:Languages\Spanish.isl"
