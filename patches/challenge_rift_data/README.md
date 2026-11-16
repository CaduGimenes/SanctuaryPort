# Challenge Rift data (offline)

Used by the "Event: Challenge Rifts" patch in `patches/Diablo_III.xml` (Diablo III, CUSA00242 1.47).

Copy these files to `user/data/pubfiles/` in your SanctuaryPort folder. The patch makes the game read
`challengerift_config.dat` and `challengerift_0..9.dat` from there instead of asking the server.

- `challengerift_0..9.dat` come from d3hack's `rift_data` (challengerift_00..09.dat). Check d3hack's license before redistributing them.
- `challengerift_config.dat` is hand-built (challenge never expires); the patch randomises the challenge number on every launch.
