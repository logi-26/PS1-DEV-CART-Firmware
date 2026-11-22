# Starts TOCA 2 (Europe) in Redux "t" and goes to a 2-player single race at Thruxton (research/toca2/NOTES.md):
# loading prerace.sst from the intro or the menus closes Redux, from inside GAME.EXE it works.
python dd_run.py t --game toca2
sleep 30
sh nav_t.sh START 3 x >/dev/null; sh nav_t.sh START 4 x >/dev/null; sh nav_t.sh START 4 lang  # intro, title
sh nav_t.sh CROSS 3 x >/dev/null                                   # language: ITALIANO
sh nav_t.sh none 2 x >/dev/null
sh nav_t.sh CROSS 2 x >/dev/null                                   # MENU PRINC.: INIZIA GARA
for i in 1 2 3; do sh nav_t.sh DOWN 0.8 x >/dev/null; done
sh nav_t.sh CROSS 2 x >/dev/null; sh nav_t.sh DOWN 1 x >/dev/null   # CORSA SINGOLA, cursor on GIOCATORI
sh nav_t.sh RIGHT 0.8 x >/dev/null                                 # 2 players
for i in 1 2 3; do sh nav_t.sh DOWN 0.8 x >/dev/null; done
sh nav_t.sh RIGHT 0.8 x >/dev/null                                 # no qualifying
for i in 1 2; do sh nav_t.sh DOWN 0.8 x >/dev/null; done
sh nav_t.sh CROSS 2 x >/dev/null                                   # CONTINUA
sh nav_t.sh CROSS 0.8 x >/dev/null; sh nav_t.sh START 2 x >/dev/null       # player 1's name
sh nav_t.sh 2:CROSS 0.8 x >/dev/null; sh nav_t.sh 2:START 2.5 x >/dev/null # player 2's name
sh nav_t.sh CROSS 3 x >/dev/null; sh nav_t.sh CROSS 30 x >/dev/null       # track, difficulty, load
sh nav_t.sh CROSS 2 x >/dev/null; sh nav_t.sh 2:CROSS 8 nav_race          # FATTO, INIZIA CORSA
