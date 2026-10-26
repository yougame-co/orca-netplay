# Standard disc layouts for orca-disc-tool

`orca-disc-tool --rebuild-standard <image> --layout RSBE01-rev2.layout.json --against RSBE01-rev2.ref.json --out <out.rvz>`
puts a player's rearranged Brawl (USA) Rev 2 copy back in the standard layout.

Made from the team's own Brawl Rev 2 disc (a copy of a disc Luis owns) with `orca-disc-tool --disc-layout` and
`--disc-report` (e001c69cec), 2026-10-26. Contents, as cleared by compliance (#13276, #18578): names, offsets, sizes,
structure, generator seeds, subtree digests of erased clusters, and the two content digests (layout); paths, sizes and
SHA-1s of the game partition's files (reference). No disc bytes, raw FST, TMD, ticket or certificate data.
Rebuilds verify against the H3/TMD digests before the output is kept.
