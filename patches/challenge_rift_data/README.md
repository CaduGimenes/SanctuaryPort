# Challenge Rift data (offline)

Used by the "Event: Challenge Rifts" patch in `patches/Diablo_III.xml` (Diablo III, CUSA00242 / CUSA00243 1.47).

The game normally downloads the Challenge Rift definitions from Blizzard's servers as "pubfiles":
`challengerift_config.dat` and `challengerift_0.dat` .. `challengerift_9.dat`. They are not part of the
game's packages, so SanctuaryPort does not ship them (no game data is distributed with the project).

To play Challenge Rifts offline, put your own copies of those files in `user/data/pubfiles/` in your
SanctuaryPort folder (create the folders if they do not exist) and tick "Event: Challenge Rifts" in the
launcher. The patch then makes the game read them from there instead of asking the server. The challenge
number is the current week (it changes every Thursday at 00:00 UTC), and the game loads
`challengerift_<number % 10>.dat`, so the ten files take turns week by week. As on the real servers, the
Challenge Rift cache is earned once per challenge number: beat the target time and the "Challenge Rift
Reward" box in town gives the cache; another run in the same week shows the reward as already earned.
Without the files the game asks its (offline) service as before and no Challenge Rift is offered.
