# Probe only: puts Start and Search back as Windows draws them, by restarting both processes without the TAP.
Stop-Process -Name startglass -Force -ErrorAction SilentlyContinue
Stop-Process -Name StartMenuExperienceHost, SearchHost -Force -ErrorAction SilentlyContinue
'Start and Search restart on their own and look as Windows draws them.'
