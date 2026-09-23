# Changelog

All notable changes are here. The format follows Keep a Changelog, and the
top section's heading is what the release workflow reads: `## [X.Y.Z] — DATE`
on the default branch publishes that version, `## [Unreleased]` publishes only
`edge`.

## [1.9.7] — 2026-09-23

### Fixed

- **Black bars: a face drawn across the screen past its own edge.** The
  `surface` reading on the re-release start map found it. The cliff face that
  belongs at the crosshair was visible and in the edge list, but a crate face
  (`crate0_side`) with an earlier sort key covered the pixel. The ray met the
  crate's plane outside the crate itself. The crate's span had lost a closing
  edge and run on sideways: a bar of a dark texture.

  The renderer skips clipping against a screen edge for every face in a BSP
  node whose bounds lie wholly inside that edge. It also shares edges between
  faces within a frame: an edge one face found wholly off-screen is skipped by
  the others that use it. Both rely on every face lying inside its node's
  bounds, which id's compiler guaranteed by cutting faces along the tree.
  Modern compilers can leave faces uncut (ericw-tools' `func_detail_wall` and
  `func_detail_illusionary` exist for that), and such a face can stick out of
  its node. It then goes unclipped against an edge it crosses. A neighbour
  that did clip marks their shared edge as off-screen, the face skips it, and
  nothing closes the span.

  Node bounds are now widened on load to hold their own faces and the nodes
  below them. When that changes anything, the console says so once per map,
  which is how we will know this was it. id's maps need no widening and render
  pixel for pixel as before. In a test build with every node's bounds shrunk by
  64 units, widening restored every frame exactly.

## [1.9.6] — 2026-09-23

### Added

- **`surface` explains a mismatch.** When the face drawn at the crosshair
  isn't the one the map puts there, it now says why. For the face that
  should be there, it gives whether it was marked visible this frame and
  whether it made the edge list, with its sort key. For the face that was
  drawn, it gives the texture, how far along the ray its plane lies and
  whether the ray meets the face there, and its sort key. That tells a face
  culled by visibility from one sorted behind, and either from the probe
  missing a nearer face.

## [1.9.5] — 2026-09-23

### Added

- **`surface` also says what was drawn at the crosshair.** It now reports
  which surface the renderer gave that pixel and the colour it came out,
  alongside what the map says is there. `NO SURFACE` means a gap in the
  geometry, and `NOT that face` means the wrong face was sorted in front.
  Either points at the renderer; a match with a black pixel points at the
  texture or the light. Bound to a key (`bind p surface`), it reads the view
  with the console up.

## [1.9.4] — 2026-09-23

### Added

- **`surface`, a console command for reporting what is drawn wrong.** Aim at
  it and type `surface`. It names the face under the crosshair and its model
  (world or door, lift, wall), the texture and how dark its pixels are, and the
  lightmap at that point: which styles, the samples, and the light that comes
  out. This separates a gap in the geometry from black texture art from a
  lightmap that says black.

### Fixed

- **Faces lit by a light style above 63 were black.** A face can name any
  style up to 254, but only 0 to 63 are ever set, and the rest stayed at zero
  brightness in this renderer. GLQuake gives them normal light, as it does an
  unset style below 64. They now get it here too.

- **A lightmap that runs past the end of the map's light data** is caught on
  load, counted with the other bad references, and drawn at full brightness.
  Before, it read whatever followed in memory.

- **A black box behind the version text at the bottom right of the
  console.** id's console art has a dark plate under the id logo, sized for
  the four characters DOS Quake stamps on it: `1.09`. The X11 build stamped
  `(X11 Quake 1.10) 1.09` ending at the same margin. Only the last four
  characters landed on the plate, and the rest ran across the texture to its
  left, so the plate looked like a stray black bar. It now gets the DOS stamp.
  The port's own version is on the launch page. A game directory whose console
  picture isn't id's 320x200 is no longer written into at id's offsets.

## [1.9.3] — 2026-09-23

### Fixed

- **Slivers of doors, lifts and walls stretched across the screen.** A brush
  model that spans more than one part of the world is cut along the world's
  planes before it is drawn. A face crosses a plane twice or not at all, and
  the two cut points are joined by a new edge. Each vertex is measured against
  the plane twice, as the end of one edge and the start of the next, and id
  wrote that measurement out twice. Under `-ffast-math` the compiler may work
  the two copies out differently, so a vertex lying on the plane could come out
  in front one time and behind the other. The face then crossed the plane only
  once. The closing edge was made from this cut's point and one left over
  from an earlier face, somewhere else entirely, and the face was drawn
  stretched out to it.

  This is id's code and id's maps do it too. `timedemo demo2`, which draws the
  same frames every run, has 71 such cuts, and now has none. The re-release
  maps have many more brush models crossing the world's planes. Each vertex is
  now measured once, by one function, so it gets the same answer both times. If
  a face still crosses an odd number of times, that piece is left out for the
  frame and the console says so once per map.

- **A crash that 1.9.2's fix could cause.** 1.9.2 leaves out a face whose
  vertex will not project. When that was the face's first vertex, the next edge
  was built from a vertex the previous face had left behind. Its slope came out
  as a NaN, and the scan walked off the edge list into a null pointer. Found by
  forcing bad clip points; with one in seven forced bad, three demos ran
  without a crash. After a bad vertex, nothing more is emitted for the face,
  and an edge whose slope is not a number is handed back.

- **Clip points are kept on their edge.** A cut point's position along its
  edge is a fraction between 0 and 1, by construction. It is now held there,
  in all three clippers. The 1.9.2 report of `(inf -nan -nan)` is what an
  infinite fraction makes from an edge that runs along one axis.

- **Big BSP29 maps read their indices signed.** Node children, face and
  leaf counts, clipnode children, and a face's plane, texinfo and edge count
  are unsigned in the file. id read them as signed, so on a map with more than
  32767 of any of them they pointed before the start of their arrays. They're
  now read the way the map compilers write them, as QuakeSpasm does. BSP2 maps
  were already fine.

- **A map that points outside itself is caught on load.** Edges naming
  vertices the map doesn't have, surfedges naming edges it doesn't have, faces
  naming planes, texinfo or surfedges past the end, and vertices or planes that
  aren't numbers are all set to 0 and counted. The console names the first one.
  id's loader trusted every index and read whatever lay past the end.

### Changed

- The report of a face left out now says what was not a number when one of
  the inputs is the cause: the world plane doing the cutting, or a brush
  model's position or angles. A brush model whose position isn't a number is
  left out rather than drawn from NaNs.

## [1.9.2] — 2026-09-23

### Fixed

- **Surfaces smeared across the screen as flat bands or streaks, on and off.**
  On the re-release maps a vertex now and then projects to a value that is not
  a number — dozens of edges in a frame on Acid Sanctuary. Every clamp in the
  projection compared it and let it through, `ceil()` turned it into
  `-2147483648`, and the range check dropped the edge. An edge is one side of
  a surface, so the surface was left open on those scanlines and ran on to the
  far side of the screen with its texture clamped at its own border: the flat
  band in one screenshot, the streak in another. The console note that said a
  dropped edge was "usually a seam or two" was wrong.

  1.6.x had already written those clamps so that a NaN would be caught, and it
  was correct C — but the build uses `-ffast-math`, which lets the compiler
  assume there are no NaNs and undo it. A NaN injected into a vertex went
  straight through in the shipped binary. The check now reads the float's bits,
  which no optimisation can reason away, and a face with such a vertex is left
  out of that frame whole: its edges are taken back so it opens and closes
  nothing. A face missing for one frame, where there was a band across the
  screen.

  With NaNs injected into one vertex in 400, frames that had bands now have
  the face's own outline missing and nothing drawn anywhere it should not be.
  With none injected, id's maps render pixel for pixel as before.

  What produces the NaN on those maps is not known yet — id's maps never do
  it, at any resolution or field of view tried. The first time it happens on a
  map, the console now says which vertex, of which model, seen from where:

  ```
  3 face(s) left out of a frame: a vertex projected to a value that is
  not a number. First one: vertex (x y z) of maps/..., seen from (x y z) ...
  ```

  That line is what will find it.

## [1.9.1] — 2026-09-23

### Fixed

- **Some grates were still solid pink, at any distance.** 1.8.1 takes a fence
  out of the renderer's edge list where a face normally goes in —
  `R_RenderFace`. A brush model (a `func_wall`, a door, a platform) whose
  bounds cross more than one part of the world's BSP tree never goes through
  there: its faces are first cut along the world's planes, and the pieces go
  into the edge list by a different function that had never heard of fences.
  So a walkway built from world brushes looked right, and the one next to it
  built as a `func_wall` came out as pink stripes.

  That face now goes to the fence pass whole, before it is cut. The cutting
  only exists so the edge list can sort the pieces against the world, and the
  fence pass sorts with the z-buffer instead.

  Reproduced by forcing every brush model down that path with a door made a
  fence: 61872 pink pixels before, none after, and the corridor behind the
  door shows through it. A door in the same view that takes the path on its
  own went from 2944 pink pixels to none.

## [1.9.0] — 2026-09-22

### Fixed

- **MG1's second episode stopped with `PF_precache_model: overflow`.** A map
  can load at most 256 models and 256 sounds in id's engine, and that is not a
  table size — protocol 15 sends a model or sound number as one byte, so 256 is
  as far as the wire can count. `mge2m1` needs more.

  The server now speaks FitzQuake's protocol 666 for a map that needs it, and
  only then. 666 is 15 with a way to send a second byte wherever 15 sends one —
  extra flag bits on the messages that carry a number, and three extra
  messages for the load-time ones — and nothing else changed. It is what
  QuakeSpasm and most engines since read, and the constants are taken from
  QuakeSpasm's source, so a demo recorded on such a map plays there too. The
  limits are now 2048 models and 2048 sounds, which is what QuakeSpasm allows.

  Every id map, and anything else under 256, still runs on protocol 15 and
  sends exactly the bytes it did before; the client reads both, and id's own
  demos still play. `developer 1` says when a map switches.

  Verified with e1m1 given 300 extra brush models and every door and platform
  pointed at one numbered above 255, which pushes every monster, item and the
  weapon in your hand past 358 as well. 400 models, protocol 666: the doors
  draw pixel for pixel as they do in the stock map, the door opens, the ogre
  behind it and the shotgun draw, and a demo of it records and plays back.
  Sound numbers above 255 are handled the same way but were not exercised:
  the test map stays at 103 sounds.

- **A "yes or no" question ignored a stop signal, and replayed old key
  presses once answered.** "Are you sure you want to start a new game?" waits
  in a loop of its own. `docker stop` raises a flag that only the main loop
  looks at, so with that question up the engine sat out the ten-second grace
  period and was killed without writing `config.cfg`. It now counts as "no" and
  the engine shuts down properly — in a tenth of a second, measured.

  Finding that turned up an older bug. The X driver's key queue handed an
  event on before stepping past it, and the question pumps that same queue
  from inside the key handler — so once answered, the queue looked full and
  every key press in its 64-slot ring was replayed. It now steps past first.
  The loop also no longer spins a core at 100% while it waits.

### Added

- **Backspace goes back a menu level**, and closes the menu from the top one.
  In the browser `Esc` belongs to the pointer lock, so there was no key that
  did this. Backspace keeps its own job where it has one — the name and address
  fields, and the key bindings screen, where it clears a binding — and answers
  "no" to a yes-or-no question. Holding it goes back one level, not all of them.

## [1.8.2] — 2026-09-22

### Fixed

- **A grate showed its holes up close and went solid at a distance.** 1.8.1
  made index 255 see-through, but only the first mip level of a fence texture
  has any. The smaller levels were built by the texture tool, which averages
  each block of texels into one colour and has no idea 255 means "not here" —
  so in them the holes are filled with a blend of the bars and 255's pink,
  which comes out a flat tan. The renderer switches to those levels as a
  surface gets farther away or more oblique, so a grate snapped between
  see-through and a solid sheet as you walked toward it or turned.

  Those levels are now rebuilt from the first one as the map loads. Each texel
  of a smaller level is a hole if more than half the block it covers is, and
  otherwise takes the most common solid colour in that block — a colour
  already in the texture, so fullbright texels stay fullbright and nothing
  needs a palette search.

  Measured with a fence whose holes are only in its first level, the way the
  tools leave them, counting pixels that show what is behind it: forcing the
  second-smallest level with `d_mipcap 2` gave 1082 before and 30972 after,
  against 31746 at full detail. Only fence textures are touched.

## [1.8.1] — 2026-09-22

### Fixed

- **Grates, vines and ladders were solid pink.** A texture whose name begins
  with `{` is a fence: every texel that is palette index 255 is a hole you see
  through. id never shipped one and this renderer predates the convention, so
  it drew index 255 as what it literally is — palette entry 255, a flat dusty
  pink (159, 91, 83). A ceiling grate came out as a sheet of pink with the bars
  punched into it as dark shapes.

  The holes were the easy half. The hard half is that this renderer decides
  visibility with an edge list: for each screen span the nearest surface wins
  and everything behind it is never drawn at all. A fence that keeps its place
  in that list deletes the room behind it whether or not its own pixels are
  drawn, so skipping index 255 on its own would have left the holes showing
  the previous frame.

  So a fence is now taken out of the edge list entirely — the room behind it is
  drawn as if it were not there — and the fence goes on afterwards, over the
  finished picture, tested against the z-buffer the world has just written.
  That is the same order, and very nearly the same code, that sprites have
  always used here: `D_SpriteDrawSpans` already skips index 255, tests and
  writes z per pixel, and applies fog. It wants spans and a lit texture block,
  and a world surface can hand it both.

  A fence is therefore hidden by what is in front of it, hides what is behind
  it, is lit by its own lightmap and dynamic lights like any other surface, and
  fogs with the rest of the scene. It works on brush models — doors, platforms
  — as well as on world geometry.

  Measured on a map with a common wall texture turned into a fence, so roughly
  a third of the view is masked, at 1024x768, three runs each: 507/552/543 fps
  without the fence pass against 465/477/429 with it. Frames with no fence in
  them are untouched.

  Verified by turning a shareware wall texture into a fence and comparing the
  frame before and after: 34000 pixels of palette index 255 before, none after,
  and of the 34288 pixels that changed, 288 — 0.06% of the frame — were
  anything other than those. Nothing bled over nearer geometry.

  One consequence worth knowing: the palette has no alpha, so this is a hole or
  it is not. The re-release's partly transparent surfaces are drawn opaque, as
  they always were.

## [1.8.0] — 2026-09-22

### Added

- **Fog thickness is on the Options menu**, as a slider from 0 to 4 in steps of
  a quarter. It is the same `r_fogscale` the console has, and it takes effect
  on the next frame, so the fog in front of you thins or thickens as the slider
  moves. A number you have to guess at is worth less than one you can watch,
  and the right value here is a judgement about a screen rather than a fact
  about the map.

- **A map asking for a skybox now says so.** The re-release maps set a `sky`
  key on worldspawn naming a six-sided skybox, which arrived with GLQuake and
  which this renderer has none of. It was being reported as `'sky' is not a
  field` — true, and useless. It now says what was asked for and what is drawn
  instead:

  ```
  This map asks for a skybox, which this renderer has none of -- the map's own
  sky texture is drawn instead.
  ```

  `skyname`, `skybox` and `skyfog` are recognised the same way. A key the
  engine understands but cannot honour is a different thing from one it has
  never heard of, and worth different words.

- **`developer 1` reports the sky texture and its size** as a map loads —
  `Sky "sky4" is 512x256; layers resampled from 256x256 to 128x128.` When a sky
  looks wrong there was no way to ask which one it was. Silent by default.

## [1.7.1] — 2026-09-22

### Fixed

- **Fog was far too thick to see through.** 1.7.0 drew fog on a curve I guessed
  rather than the one the maps were authored against, and got it wrong twice
  over.

  The re-release's `fog` command comes from FitzQuake, which sets
  `GL_FOG_DENSITY` to **density / 64** and `GL_FOG_MODE` to **`GL_EXP2`**. So
  the fraction of fog at distance *d* is `1 - exp(-((density/64) * d)^2)`, where
  1.7.0 used a plain `1 - exp(-density * d)`. Missing the divisor and using the
  wrong exponential compounded:

  | distance | 1.7.0, density 0.05 | correct, density 0.05 |
  | --- | --- | --- |
  | 100 | 0.99 | **0.01** |
  | 500 | 1.00 | **0.14** |
  | 1000 | 1.00 | **0.46** |
  | 2000 | 1.00 | **0.91** |

  At a hundred units it was blending in 99% fog where it should have been 1%,
  which is why a room came out as a flat wall of colour.

  The distance table reaches 32768 units now rather than 4096, since fog on the
  real curve is still deepening well past where the old one had saturated.

### Added

- **`r_fogscale`**, multiplying whatever density a map sets. 1 is FitzQuake's
  curve; lower thins the fog, higher thickens it. It takes effect immediately
  and is archived. It exists because the curve above is inferred from another
  engine's source rather than measured against the maps themselves, and a
  number that can be turned beats a number that has to be rebuilt.

## [1.7.0] — 2026-09-22

### Added

- **Fog is drawn.** 1.6.3 added a `fog` command that accepted the re-release
  maps' settings and did nothing with them. It draws them now.

  Fog arrived with GLQuake, where it is a blend the hardware does per pixel
  between the fragment's colour and the fog colour. This renderer has no
  colours to blend — it writes palette indices, and the only way to darken or
  tint one has always been a table saying what some other index looks like.
  That is exactly what `gfx/colormap.lmp` is: 64 rows of "this index at this
  light level is that index". So fog is another such table, built the same way
  and used the same way — 32 rows, one per depth band, each saying what every
  palette index becomes blended that far toward the fog colour.

  Depth comes from the `1/z` the drawers already carry, sampled where they
  already recompute it: every eight pixels for a world span, per span for a
  model or a sprite, per particle. All of them go through the same table —
  walls, water, brush models, monsters, the view weapon, sprites, particles
  and the lone vertices the subdivided model path leaves behind — so nothing
  stands out unfogged against a fogged scene.

  **It costs nothing measurable.** Timed with the engine's own `timerefresh` at
  1024x768, three runs each: 497/420/466 fps without fog against 480/467/408
  with it. The table lookup disappears into memory traffic that was already
  there. With no fog set the drawers take the same path they always did.

  Index 255 maps to itself at every level, since it is the transparency index
  in skins and sprites and a fogged monster should not grow a fogged outline.

### Fixed

- **A sky texture that is not exactly 256x128 was read as garbage.** `R_InitSky`
  splits the sky into its two layers with `256` and `128` written into it as
  constants — it never looked at `mt->width` or `mt->height` at all. Every sky
  id shipped is 256x128, so this held for thirty years.

  A re-release map's sky is bigger. At 512x256 the old code read the top-left
  quarter of the image at the wrong stride; below 256x128 it read past the end
  of the texture entirely. The layers are resampled from the texture's real
  dimensions now.

  Measured against a synthetic sky carrying a known vertical ramp, comparing
  the old routine and the new one over the same texture:

  | sky texture | id's loader | fixed | the texture's actual range |
  | --- | --- | --- | --- |
  | 256x128 | 16..79 | 16..79 | 16..79 — buffers byte-identical |
  | 512x256 | 104..31 | 16..79 | 16..79 |
  | 1024x512 | 102..19 | 16..79 | 16..79 |

  At id's size the two agree exactly, so nothing changes for the original game.

## [1.6.3] — 2026-09-22

### Added

- **A `fog` command, so the re-release maps stop reporting an error for
  something that is not wrong.** Every one of them sets fog through the progs on
  every level load, and this engine has none, so every level load printed
  `Unknown command "fog"`.

  Fog arrived with GLQuake. A renderer that writes palette indices into an
  8-bit buffer has nowhere to put it — the blend would have to happen in the
  colormap, per distance, per frame. So the command exists, takes the arguments
  it is given, and keeps them: `fog` with no arguments reports the values and
  says once that nothing is drawn from them. An error for a thing the engine was
  never going to do is worse than silence.

### Fixed

- **`-nosound` printed `Unknown command "volume"` and then lost the setting.**
  The sound cvars were registered after the `-nosound` early return, but
  `config.cfg` sets `volume` and `bgmvolume` whether or not there is sound — so
  turning sound off for one run silently reset them for the next. They are
  registered before the return now.

## [1.6.2] — 2026-09-22

### Fixed

- **The flickering RAM icon was the surface cache thrashing, and it was the
  stall.** `SCR_DrawRam` draws that icon in the top left whenever
  `r_cache_thrash` is set, which means a frame needed more lit surface than the
  cache holds — so blocks built earlier in the *same frame* were thrown away and
  have to be rebuilt, every frame, for as long as you stand there.

  id sized it at 600 KB plus 3 bytes a pixel: 1818 KB at 800x600, exactly what
  the log prints. That was generous in 1996. It is 8 MB plus 16 bytes a pixel
  now, capped at 48 MB — 14 MB at 800x600, 40 MB at 1920x1080 — and it is
  reported in words once per map, because the icon is off whenever
  `scr_showram` is and means nothing to anyone who has not read `d_surf.c`.
  `-surfcachesize <kb>` still overrides it.

- **The bmodel clipping buffers were still too small.** 1.6.1 raised them to
  8192 and 16384 and the machine campaigns still exceeded them, so a door or
  lift was still going undrawn. They are 65536 and 131072, and no longer on the
  stack: `R_DrawSolidClippedSubmodelPolygons` is called once per entity and is
  not recursive, so one static pair serves it — 786 KB and 3 MB of bss instead
  of a 4 MB stack frame.

- **Sounds from entity 4096 and above were attributed to the wrong entity —
  a regression from raising `MAX_EDICTS` in 1.6.0.** `SV_StartSound` packs the
  entity number and channel into one short as `(ent << 3) | channel`, and
  `MSG_ReadShort` sign-extends. At id's 600 edicts the largest value was 4800
  and it never mattered; at 8192 an entity at 4096 or above sets the top bit and
  comes back negative. The field is 16 unsigned bits — the wire format is
  unchanged, only how the client reads it. The bounds check below it was `>`
  where it should have been `>=`.

- **Ambient sounds ran out of channels.** `MAX_CHANNELS` was 128, eight of them
  dynamic, leaving 118 for every fan, hum and dripping pipe in the map. Past it
  `S_StaticSound` drops the sound and printed `total_channels == MAX_CHANNELS`
  — a line naming a constant nobody outside the engine has heard of, once per
  dropped sound. Now 1024, said once per map and in words.

### Added

- **"Illegible server message" now says what it choked on.** It means the
  reader is no longer on a message boundary, and on its own it names neither
  the opcode it found nor where. It now prints the opcode, the byte offset, the
  message size, and the last opcode that parsed cleanly — which is usually the
  one whose handler read the wrong number of bytes.

## [1.6.1] — 2026-09-22

### Fixed

- **A brush model too complex to clip was dropped, and said so hundreds of
  times a frame.** Reported from the machine campaigns on 1.6.0 as a console
  full of `Out of edges for bmodel`, with the picture stalling for up to 700 ms
  at a time.

  `MAX_BMODEL_VERTS` (500) and `MAX_BMODEL_EDGES` (1000) are the scratch buffers
  `R_DrawSolidClippedSubmodelPolygons` clips one brush model in — a door, a
  platform, a lift, any moving world geometry. Over the limit
  `R_RecursiveClipBPoly` returns and the model is not drawn at all: a door that
  is simply absent, not a door with a hole in it.

  They are 8192 and 16384 now, which is 98 KB and 393 KB of a stack frame that
  is not recursive. The message is said once per map instead of once per
  clipped polygon — the printing was itself slow enough to be felt, so this is
  half the stall as well as all of the noise. The vertex limit said nothing at
  all before; it reports now too.

- **The surface pool was still too small.** 1.5.1 raised it to 32768 on the
  strength of id's maps. The machine campaigns come up **4128 short of that on
  a single frame**, which is geometry missing from the view. 65536 now, which is
  the most it can safely be: `edge_t` holds the index of the surface an edge
  belongs to in an `unsigned short`, so a surface at index 65536 or above is
  written back as a different one — it does not fail, it draws the wrong
  surface. Raising `r_maxsurfs` past that is now clamped and reported, since the
  shortage message tells the reader to raise it. The edge pool was not short on
  the same frame and is unchanged.

- **Three writers into the signon message, one of them still able to exit the
  engine.** `PF_makestatic`, `SV_CreateBaseline` and `PF_ambientsound` all write
  into a fixed buffer whose `allowoverflow` is clear, so filling it is
  `Sys_Error` from `SZ_GetSpace` with the map nearly loaded — the worst place
  for a hard stop.

  All three check for room now and drop what will not fit, once with an
  explanation, so a map that is too big for the protocol loses some torches or
  ambient loops rather than refusing to load. Found by building maps with 400
  to 4000 extra static entities out of the shareware data: 1.5.1 exits on all
  of them, 1.6.0 exited above about 1800, and all of them now play.

## [1.6.0] — 2026-09-22

### Fixed

- **The machine campaigns hit four more limits from 1996, and three of them
  were silent.** Reported against 1.5.1 as errors and wrong colours on MG1,
  and missing geometry on MG3.

  - **Static entities.** `MAX_STATIC_ENTITIES` was 128 — torches, flames,
    anything the progs calls `makestatic()` on. The machine maps carry several
    hundred, and going over it was a `Host_Error` that dropped you to the
    console with the map half-loaded. Now 2048.

  - **Efrags.** `MAX_EFRAGS` was 640. An entity is linked into every leaf it
    touches, one efrag each, so this is not a count of entities: one static
    torch in a doorway takes several. Out of them, `R_SplitEntityOnNode`
    printed and returned, so the entity was not drawn in that leaf — a torch
    that vanishes as you walk past it. And it printed *per leaf*, which is
    where the flood of identical lines came from. Now 32768, and said once per
    map.

  - **Visible entities.** `MAX_VISEDICTS` was 256. Past it the renderer simply
    stopped accepting entities for the frame, with nothing said — the same
    silent drop as the surface and edge pools in 1.5.1, and just as invisible.
    Now 4096.

  - **Edicts.** `MAX_EDICTS` was 600, with id's own comment reading "FIXME:
    ouch! ouch! ouch!". `ED_Alloc` calls `Sys_Error` when it runs out, so a map
    with more entities than that does not load at all. The wire format was
    never the limit — an entity number goes out as a short when it needs to —
    so it is 8192 now.

  Static entities travel in the signon message, so raising the first of these
  without the buffers under it would have turned a clean `Host_Error` into a
  `Sys_Error` from `SZ_GetSpace`. The signon buffer is 48000 bytes and
  `MAX_MSGLEN` 64000 to carry it; `NET_MAXMESSAGE` is 131072, which is two of
  those, because `Loop_SendMessage` appends into that buffer and calls
  `Sys_Error` rather than refusing when the next message will not fit.
  `Datagram_SendMessage` already fragments a reliable message, so nothing in
  the protocol had to change. All of it together costs 2.1 MB resident,
  measured at 25.4 MB to 27.6 MB.

  The signon message is the real ceiling on static entities — 14 bytes each,
  plus 16 per entity with a model — which is why that one is 2048 rather than
  larger. `SV_SpawnServer` now says so in those words if a map comes close,
  instead of leaving it to an allocator complaining about a buffer nobody has
  heard of.

  `MAX_MODELS` and `MAX_SOUNDS` stay at 256. Those really are wire-format
  limits — the indices go out as bytes.

- **The engine crashed at startup on a machine with no sound.** `S_Startup`
  leaves `shm` NULL when the device will not open, and the next line in
  `S_Init` read `shm->speed` through it — a null dereference, from 1996, in
  every build of this engine including 1.5.1. It never showed here because the
  container always has somewhere to write audio; a new smoke phase that runs
  the engine with no audio fifo found it. It now says sound is off and plays
  silent, which is what the rest of the engine was already prepared for.

- **"Short 4 edges" against a pool of 131072.** The shortage report added in
  1.5.1 counted two different failures as one. Edges dropped because the frame
  pool was full share a counter with edges dropped because a scanline index
  came out off the screen, and only the first is what `r_maxedges` controls —
  so a handful of the second read as a pool shortage and the advice that came
  with it was useless. They are counted and reported separately now, and the
  second says plainly that `r_maxedges` does not affect it.

### Changed

- **The X screen is depth 24, not depth 8.** The software renderer draws
  palette indices, and it can either write them into an 8-bit colour-mapped
  visual or translate each frame into a deeper one. It was doing the first.

  That put the palette in a private X colormap, which x11vnc then had to find,
  read, and transform the whole screen through (`-8to24`) for every client —
  the most expensive thing it did here, and a mode its own manual says "does
  hog resources". When that mapping went stale, which happens when the window
  it belongs to is replaced, the browser got the right picture in the wrong
  256 colours and did not recover. That is the magenta-and-teal screen: not
  the game drawing wrongly, the colours it drew with going missing in transit.

  At depth 24 there is no colormap anywhere in the path. Measured at 1024x768:
  430 fps at depth 8 against 360 at depth 24, and x11vnc stops doing its most
  expensive piece of work — both numbers several times what a browser can
  show. `QUAKE_X_DEPTH=8` puts the old path back.

  The smoke suite runs at depth 24 now, with a phase that starts the engine on
  the other depth so both stay covered.

## [1.5.1] — 2026-09-22

### Fixed

- **Half a level could be missing from the view, silently.** The renderer holds
  one frame's worth of geometry in two fixed pools, and id sized them for
  320x200 and for id's own maps: 800 surfaces and 2400 edges, which the whole
  shareware episode peaks at 458 and 1162 of. Past the limit `R_RenderFace` and
  `R_RenderBmodelFace` stop emitting and the geometry is simply not drawn.

  A map built for the Quake re-release has far more in view at once than
  anything from 1996, so the machine campaigns ran the pools dry and walls went
  missing. The defaults are 32768 surfaces and 131072 edges now, which moves
  them off the stack and onto the hunk — about 10 MB, measured as 15.7 MB to
  25.7 MB resident. The heap they come from went from 64 MB to 192 MB; that is
  one malloc whose untouched pages are never resident, so the virtual size grows
  and the footprint does not.

- **And it said nothing while doing it.** The two counters that notice a short
  frame were behind `r_reportsurfout` and `r_reportedgeout`, both off by
  default, so the only symptom was geometry missing from the picture with
  nothing anywhere to connect it to a limit. It is reported once per map now,
  with the current values and the names to raise:

  ```
  This frame did not fit: short 137 surface(s) and roughly 0 edge(s).
  Geometry is being left undrawn. Raise r_maxsurfs (now 64) and
  r_maxedges (now 200) and restart the map.
  ```

  Reproduced by building an engine with the pools cut to 64 and 200 and
  rendering e1m3: most of the room is black, torches hanging in the void. The
  same view with the new defaults draws the room complete.

## [1.5.0] — 2026-09-21

### Added

- **BSP2 maps load.** The Quake re-release and every modern map compiler emit
  BSP2, which is the same fifteen lumps as id's format with the indices and
  bounds widened past what a short holds. Six of them differ — nodes, leafs,
  faces, clipnodes, edges and marksurfaces — and all six are read both ways now.
  *Dimension of the Past* and the machine campaigns are the reason you would
  care.

  The in-memory structures widened with them, and the server's collision hulls
  moved off the on-disk record: id traced against `dclipnode_t` directly, whose
  children are shorts, so neither on-disk layout can be the runtime one any
  more. Both are read into an `mclipnode_t` instead. `MAX_MAP_LEAFS` went from
  8192 to 65536, which is four PVS bitvectors at 8 KB each.

  Verified by conversion rather than by assertion. `tools/bsp29to2.py` rewrites
  a BSP29 map as BSP2, and a new `bspchecksum` command walks the loaded world
  and CRCs the values — pointers turned into indices, since those are hunk
  addresses. Across all nine shareware maps the two layouts produce **identical
  checksums**, and on e1m1 the rendered frame is byte-for-byte identical and the
  player settles at the same position, which is the collision hulls agreeing.

  Comparing rendered frames alone cannot do this job: Quake animates textures
  and entities against the clock, so two runs of the *same* map do not match
  each other. That was tried first, and it is why the test compares the model.

  **2PSB**, the RMQ variant, is still not read — the engine names it rather than
  printing a number.

### Fixed

- **A cvar the progs sets but the engine does not define is created rather than
  refused.** QuakeC's `cvar_set` on an unknown name printed "there is an error
  in C code if this happens" and dropped the write. For the re-release progs
  that is neither an error nor rare: it sets `campaign` almost every frame, so
  the log filled with hundreds of identical lines and the value never came back
  when the progs read it. Engine code still goes through `Cvar_Set`, where an
  unknown name really is a bug.

- **An unknown entity field is reported once per name, not once per entity.**
  Maps built for the re-release carry `alpha` and `fog` keys that the 1996
  QuakeC does not declare, on every entity. Measured on a map with 369 such
  entities: **738 lines before, 2 after**, with a count of what was suppressed.

## [1.4.5] — 2026-09-21

### Fixed

- **A NaN in the renderer crashed the engine by writing 16 GB out of bounds.**
  `R_EmitEdge` clamps each projected vertex to the viewport and uses `ceil()` of
  the result to index the per-scanline edge arrays. Every comparison against a
  NaN is false, so a NaN passed both clamps untouched, `ceil()` returned a NaN,
  and the cast to `int` gave `INT_MIN` — 16 GB below the array.

  It arrived as a SIGSEGV in `R_EmitEdge` on a remastered Scourge of Armagon
  map, and the reported fault address was 16.00 GB below the text segment, which
  is `INT_MIN` times the size of a pointer. The clamps are negated now, so a NaN
  takes the assignment rather than skipping it, and the two scanline indices are
  checked immediately before they are used. For finite values nothing changes:
  the rendered frame is byte-for-byte identical to the previous build.

## [1.4.4] — 2026-09-18

### Fixed

- **A game that could not start took the container with it.** The engine stops
  on a `Sys_Error` — a map it cannot read, data that is not there — and that is
  not something restarting fixes, so the run loop breaks on it. But which game
  is played is a choice stored in the state volume, and the menu that sets it is
  inside the game: if that choice was what the engine died on, the container
  stopped on every start and the only way back was to edit the state volume by
  hand.

  The stored choice is dropped and the base game tried once instead, with the
  reason in the log. An explicit `-game`, `-hipnotic` or `-rogue` in
  `QUAKE_ARGS` is left alone, because it applies again on the next start
  whatever the stored file says — dropping it would throw away the player's pick
  and change nothing else. A clean quit and a crash are unaffected: they already
  restart on the same game.

- **BSP2 maps now say what they are.** `Mod_LoadBrushModel` reported them as
  `wrong version number (844124994 should be 29)`, and that figure is the four
  bytes `BSP2` read as an integer. BSP2 is a map format from long after 1996 —
  32-bit node and leaf indices, a larger visibility lump — and it is what the
  Quake re-release ships, so anyone taking an episode out of Steam rather than
  off the CD will meet it. *Dimension of the Past* is the usual way. The engine
  names the format and says this renderer only reads the original version 29.

  Reading BSP2 is not something this port does. It is the 1996 software
  renderer, and the format exists precisely to get past what that renderer
  assumes.

## [1.4.3] — 2026-09-18

### Fixed

- **One stray pak file could hide every mission pack and mod.** The data mount
  was checked for pak files sitting loose at the top *first*, and the scan for a
  directory per game was the `else` branch — so a single `pak0.pak` beside a
  perfectly good `id1/`, `hipnotic/` and `rogue/` took the whole mount over. It
  became id1, nothing else was ever looked at, and the game directories were
  linked into id1 as loose files. The symptom was a Game menu holding nothing
  but Quake.

  The per-directory scan runs first now, and the loose-paks guess only when that
  found no base game — so the documented layout always wins and the convenience
  still works for a mount that is nothing but pak files.

- **A mod added while the container was running never appeared.** The mount was
  scanned once, at container start, but the engine restarts whenever the player
  quits or picks a different game — so anything dropped in between was invisible
  until a `docker restart`. The scan runs before each start of the engine now,
  and says so in the log only when the answer changed.

- **A game directory that lost its pak files took the player's savegames with
  it.** The directory is removed when it turns out to hold no game data, which
  is right for a directory the script has just made and wrong for one the engine
  has been writing `config.cfg`, savegames and screenshots into. It is left
  alone now, with a line in the log saying why.

## [1.4.2] — 2026-09-18

### Fixed

- **A mission pack played Quake's music instead of its own.** The soundtracks
  differ — Scourge of Armagon and Dissolution of Eternity have their own — and
  a rip of each belongs beside its own pak files, in `hipnotic/music`,
  `rogue/music` and so on. The engine does look there. It just looked at
  `$QUAKE_MUSICDIR` first, and the entrypoint set that to `id1/music` whenever
  that directory existed, so from then on every game played Quake's tracks.

  `$QUAKE_MUSICDIR` can only ever name one directory for every game the
  container can run, so it is the cross-game default and the game's own
  directory now beats it. The full order is `-musicdir`, then `<game>/music`,
  then `$QUAKE_MUSICDIR`, then `id1/music` — the last so a mod with no music of
  its own still gets Quake's. The entrypoint no longer points
  `$QUAKE_MUSICDIR` at `id1/music` at all; that fallback was already in the
  engine, and setting it was what did the damage.

- **An empty `music` directory won and played nothing.** The check behind the
  search was `S_ISDIR`, despite being called `CDAudio_DirHasFiles`, so a
  directory with no tracks in it was taken and the fallbacks below were never
  reached. It looks for a `track*` file in a format libsndfile reads now, and
  falls through when there is not one. The container links a `music` directory
  into every game directory it finds, so an empty one is easy to end up with.

- The startup log now says which games have music of their own, rather than
  naming one directory and leaving the rest to be guessed at.

- The smoke test grew a phase for it: 880 Hz in the game's own music directory,
  440 Hz in the shared one, `$QUAKE_MUSICDIR` pointed at the shared one, and an
  assertion that what comes out of the mixer is 880. It fails on the previous
  build and passes on this one.

## [1.4.1] — 2026-09-18

### Fixed

- **The sound stopped for good after any frame longer than three quarters of a
  second — which every level load is.**

  `GetSoundtime` worked out where playback had got to by counting the times the
  ring buffer wrapped, one wrap per call, and id's own comment above it says
  what is wrong with that: *"it is possible to miscount buffers if it has
  wrapped twice between calls to S_Update. Oh well."* The ring is 16384 frames,
  0.74 s at 22050 Hz. In 1996 a frame that long meant the machine had stopped.
  Here a level load is that long, and a browser on a busy machine is worse —
  this port logs picture gaps of one to eighteen seconds as an ordinary
  occurrence.

  A lost wrap is lost for good, because nothing ever recounts. `paintedtime`
  follows `soundtime`, and `SNDDMA_Submit` will not send past `paintedtime`, so
  from then on every frame went down the pipe as silence rather than as the
  mixer's output. At exactly the right rate — so the browser's buffer never
  underran, the page reported nothing, the container reported nothing, and the
  game simply went quiet and stayed quiet.

  Measured on a demo loop: a 3.5 s level load left `paintedtime` 63331 frames
  behind the clock, and it was still exactly 63331 frames behind twenty seconds
  later. With the same engine stalled deliberately for four seconds, the audio
  captured off the pipe was full-scale for twelve seconds and then flat zero for
  the remaining fourteen.

  There is no sound card here and nothing to reconstruct: this backend's clock
  *is* the playback position. `SNDDMA_GetSamples` hands the running count over
  and `GetSoundtime` stops guessing. Same test, same stall: audio the whole way
  through.

  The 0.04 s floor on the sound-delay slider in 1.4.0 was not this. It was a
  guard put up while the cause was still unknown, and it stays because 0.02 s
  is genuinely too little for one frame's grace — but it fixed nothing, and
  this is what was actually wrong.

- The smoke test grew a phase for it: a sustained tone, `SIGSTOP` for three
  seconds, and an assertion that the tone is still arriving five seconds after
  the engine is let go. It fails on the previous build and passes on this one.

- **The quicksave was the one save the load menu would not show.** F6 and F9 are
  bound to `save quick` and `load quick`, which writes `quick.sav`; the menu only
  ever looked for `s0.sav` to `s11.sav`. In 1996 that was survivable, because F9
  was right there. In a browser it is not — the page may never see F9 at all, and
  a player who had quicksaved had no way back to it.

  It is a thirteenth row now, in both the load and the save menu, after a blank
  line so it reads as separate and drawn in white rather than gold so it is still
  identifiable once it holds a real comment.

## [1.4.0] — 2026-09-18

### Added

- **Mission packs and mods are in the menu.** **Options → Game / mission pack**
  lists everything installed beside `id1` — Scourge of Armagon, Dissolution of
  Eternity and the mission packs' own name for anything else, by directory.
  Picking one restarts Quake on it.

  It restarts rather than switching in place because the search path is built
  once, in `COM_InitFilesystem`, and nothing rebuilds it: swapping it underneath
  a running game means throwing away every model, sound, texture and progs the
  hunk holds and loading them again, which is most of what starting over does
  anyway with none of the certainty. In the container the restart is close to
  invisible — the engine already runs in a restart loop and the page reconnects
  by itself, so it is a few dark seconds. Started by hand it quits, and the
  choice applies next time.

  The choice is stored beside the game directories and read at startup, and it
  applies exactly what the switch for that directory would have: `rogue` and
  `hipnotic` are not only search paths, they change the status bar and the menu.
  An explicit `-game`, `-hipnotic` or `-rogue` still wins, because the engine
  reads its command line before it reads the file.

- **`QUAKE_GAME` is a starting point rather than a lock.** Setting it, or
  changing it, overrides whatever the menu last chose; leaving it alone leaves
  the menu's choice alone. Without that, a container configured with
  `QUAKE_GAME=hipnotic` would drag the game back to Scourge of Armagon on every
  restart and the new menu would appear to do nothing.

- **The sliders say what they are set to.** A slider shows how far along it is,
  which for a field of view or a mouse speed is not the number anybody wants.
  Printed to the right of the bar now, with as few decimals as the value needs.

### Fixed

- **The weapon disappeared above a 90 degree field of view.** id's own guard:
  `R_DrawViewModel` returned on `r_fov_greater_than_90`, because the gun is a
  foot from the camera where a wide projection stretches it across the screen
  and out through the wall behind it. Fine when the field of view was a console
  command nobody found; not fine now that it is a slider, where moving it made
  the weapon vanish with no explanation.

  Answered the way every later port answers it: the view model gets its own
  field of view, fixed at 90 and widened for the screen exactly as the world's
  is, and the world keeps the player's. The software renderer reads the
  projection out of four globals, and nothing but the view model is drawn
  between saving them and putting them back.

- **The sound delay slider went low enough to starve the mixer.** Quake paints
  one buffer per frame, so `_snd_mixahead` is also how long a frame may take
  before the sound runs dry; 0.02 s is 20 ms, which no browser-in-a-container is
  going to hold to. The slider starts at 0.04 now. Anything lower is still there
  at the console for somebody who means it.

## [1.3.0] — 2026-09-18

### Added

- **The options menu covers the settings, rather than thirteen of them.**
  Twenty-three rows now, scrolling the way the controls menu does: the field of
  view, mouse look, smooth mouse, the crosshair, whether the weapon is drawn,
  view bob, view kick, water warp, texture detail and the sound delay, next to
  everything that was already there.

  id's menu drew each row in one switch statement, adjusted it in a second and
  acted on Enter in a third, with the row's identity being its position in all
  three — so adding a setting meant editing three places and getting the
  numbering right in each. Everything id added after 1996 went to the console
  instead, and so had everything this port added: `freelook`, the sound delay,
  the field of view that widescreen made worth changing. One table now says
  what each row is and which cvar it moves, and adding a setting is one line.

### Fixed

- Six of the settings now in the menu were not archived cvars, because id only
  ever offered them at the console and a console setting was not expected to
  last. A row in a menu is: set it, and it is still set tomorrow. `fov`,
  `r_drawviewmodel`, `cl_bob`, `v_kicktime`, `r_waterwarp` and `d_mipcap` are
  archived now.

- **`d_mipcap` did nothing until the resolution changed.** It caps how blurry
  the far end of a wall may get, and dropping detail is one of the few things
  that buys frames in a software renderer — but it was read once, in
  `D_InitCaches`, which runs on a video mode change and at no other time. Read
  per frame now, which is a float and a clamp against everything else a frame
  does.

- A menu row whose cvar does not exist says `n/a` and does nothing, rather than
  printing `Cvar_Set: variable volume not found` once per press of an arrow
  key. `-nosound` makes `S_Init` return before it registers `volume`,
  `bgmvolume` and `_snd_mixahead`, so the three sound rows had nothing behind
  them on a run with the sound off. id's menu did that too.

## [1.2.0] — 2026-09-18

### Fixed

- **Widescreen modes were stretched.** `vid.aspect` is the shape of one pixel,
  and the renderer multiplies the vertical scale by it. id computed it as
  `(height / width) * (320 / 240)`, which cancels to a constant 4:3 whatever
  the mode is — correct in 1996, when every mode was 4:3 and 320x200 really was
  displayed with non-square pixels. Every mode here is a framebuffer in a
  browser, where a pixel is square, so the renderer drew a 4:3 picture and the
  browser showed it at 16:9. Measured as the ratio of the renderer's vertical
  to horizontal scale: 1.0 at 4:3, 0.83 at 16:10, 0.75 at 16:9 — a quarter too
  wide, which is exactly the mode's aspect over 4:3.

  A wider screen now also shows more to the sides rather than cropping the top
  and bottom off, which is what a fixed horizontal `fov` would otherwise do:
  the horizontal field of view is widened by however much wider than 4:3 the
  screen is, leaving the vertical one where 4:3 puts it. Nothing changes at 4:3
  or narrower — 640x480 renders the same scale factors it always did — and
  `fov` still means the horizontal angle at 4:3.

- **The sound lagged further behind than it needed to.** The engine mixes ahead
  of itself, and id's `_snd_mixahead` of 0.1 s is most of the delay between
  firing a shot and hearing it. Measured from a keystroke to the sound reaching
  the socket: 130 ms at 0.1, 84 ms at 0.06, 72 ms at 0.04, and no further gain
  at 0.02 — below about 0.04 the floor is one engine frame plus the chunk size.
  The container asks for 0.06, which takes the 46 ms that is really there;
  `QUAKE_SND_MIXAHEAD` moves it, and 0.1 is id's behaviour exactly.

  Passed as a console command rather than seeded into `config.cfg`, because the
  cvar is archived: a state volume that already exists has id's 0.1 in its
  config and would overrule anything written there.

- The page says in the container log when its own buffer grows, and by how
  much. It answers an underrun by holding more sound — a longer delay beats a
  click — and on a machine that cannot keep up that climbs to 250 ms and
  becomes most of the delay. The figure existed only in `window.__audio()`, so
  a report of delay arrived with no way to tell a grown buffer from a container
  sending late.

- **The engine crashed while reporting that it could not open the display.**
  `VID_Init` answers a display it cannot open with `Sys_Error`, which calls
  `Host_Shutdown` and so `VID_Shutdown` — the one path where there is no
  display to close. `XAutoRepeatOn` then dereferenced a null `Display` and the
  process died on `SIGSEGV` at `0x968`.

  The real message had already been printed, but what the container saw next
  was a segfault, so it reported a crash, restarted, crashed identically, and
  gave up three runs later — with the signal in the log and the reason sitting
  above it looking like part of the previous run. That is the shape of the
  startup crash reported against 1.0.0 and never explained.

- The container starts the engine at the resolution `config.cfg` asks for
  rather than at `QUAKE_WIDTH`/`QUAKE_HEIGHT` and resizing into it a frame
  later. `vid_width` is archived and `config.cfg` is exec'd after `VID_Init`,
  so the window was always created at one size and resized to another; now it
  is not, which removes a browser reconnect from every start. Re-read before
  each run, because the engine rewrites `config.cfg` when it exits.

  On its own this is not what fixes the colours below; it removes a gratuitous
  resize, which is worth having anyway.

- **The picture came back in the wrong colours after quitting to the title
  screen.** Teal and magenta instead of Quake's browns, and it stayed that way.

  The engine exits, the container starts it again, and the new window has a
  colormap of its own. x11vnc shows this 8-bit screen to the browser by
  converting it through the window's colormap, and it carried on converting
  through the one that died with the old window. x11vnc's manual owns the
  limitation: "if there are multiple 8bpp windows using different colormaps,
  one may have to iconify all but one for the colors to be correct."

  A VNC session that connects afresh is correct every time; nothing else tried
  was — not `x11vnc -R refresh`, not `-fixscreen 8=t`, not re-uploading the
  palette from the engine, not starting the engine at the config's resolution.
  So the container counts engine starts, serves the count at `/quake-run`, and
  the page opens a new session when it moves, about five seconds after a
  restart. Nothing in the VNC protocol says a window was replaced, so it has to
  be counted rather than noticed.

## [1.1.0] — 2026-09-17

### Added

- **WASD and mouse look by default.** The container seeds `config.cfg` once,
  on a state volume that has none, with W A S D to move, the mouse to look, the
  wheel to change weapon and E/Q to swim. id's 1996 defaults — arrow keys to
  move, `,` and `.` to sidestep, `a` to look up, and the mouse walking you
  forward unless you hold `\` — are still one `QUAKE_MODERN_CONTROLS=0` away,
  and the engine owns the file afterwards, so anything changed in the game
  persists over them.
- **A `freelook` cvar**, archived and set to 1, so the mouse steers the view
  with no key held. `+mlook` is untouched and still wins while it is held;
  `freelook 0` is the 1996 behaviour exactly. Every place that asked
  `in_mlook.state & 1` for this question now asks one macro, so the two cannot
  drift apart.
- **Mouse wheel support in the X11 driver.** X delivers the wheel as buttons 4
  and 5; the 1996 code handled three buttons and dropped the rest, while
  `keys.c` had `K_MWHEELUP` and `K_MWHEELDOWN` in it the whole time. Sent
  straight to `Key_Event`, because a notch is momentary and `IN_Commands` only
  reports changes between frames.
- **Customize controls covers everything**, thirty-one actions rather than
  eighteen, including the weapon keys, the console, the scoreboard, pause and
  screenshot. The old limit was the screen: eighteen rows is all that fits, so
  the menu scrolls now, with indicators for which way there is more.
- **A Video Options menu**, with twenty resolutions from 320x240 to 1920x1200.
  The X11 driver never set `vid_menudrawfn`, and `menu.c` hides the line when
  it is null, so the X build had no video menu at all — the resolution was
  whatever the command line said and nothing could change it afterwards.
  Picking a mode writes the archived `vid_width` and `vid_height` cvars, which
  is also what makes the choice survive a restart; setting them at the console
  does the same thing. Modes the X server will not accept are not offered.
- **The engine resizes the X screen, not just its window.** The browser sees
  the whole root window, so a smaller window would sit in the corner of a
  framebuffer it cannot fill. The engine creates the RANDR mode and moves the
  screen to it, and x11vnc's `-xrandr resize` passes the new size to the
  browser as NewFBSize — which noVNC handles by resizing its canvas, visible
  as a brief blink. A server's maximum screen size is fixed when it starts, so
  the container now starts Xvfb at 1920x1200 and the engine brings it down;
  `QUAKE_MAX_WIDTH` and `QUAKE_MAX_HEIGHT` change that ceiling. Only done when
  `-resizescreen` says the engine owns the display, because on a desktop
  picking a resolution in Quake has no business rearranging anything else.

### Fixed

- **Only the first shifted character of a session reached the console.**
  `XLateKey` took the keysym with the event's shift state applied, so
  shift+minus arrived as `_` going down and — shift being up by then — as `-`
  coming up. `Key_Event` counts autorepeats in `key_repeats[key]` and only
  clears the entry on the release, so `key_repeats['_']` went to 1 and stayed
  there, and every `_` after the first was discarded as an autorepeat. The
  same for every capital letter, colon and quote: `vid_width` reached the
  console as `vidwidth`, and a name or a server address could be typed once.
  The keysym is now taken with shift masked out, which is what `keys.h` asks
  for ("normal keys should be passed as lowercased ascii") and leaves the
  shift table in `keys.c` to do its job.
- **A resize crashed the engine when the new mode was larger.**
  `D_InitCaches` announces the new surface cache size with `Con_Printf`, and
  `Con_Printf` draws the screen — which re-entered `SCR_UpdateScreen` from
  inside `VID_Update`, with `vid.width` already the new size and `vid.buffer`
  still the old, smaller framebuffer. `Draw_ConsoleBackground` then wrote a
  640-pixel row into a 512-pixel one. `block_drawing`, which `SCR_UpdateScreen`
  has always checked first thing and which `vid_win.c` sets around a mode
  change for this exact reason, is now set here too; nothing in this build had
  ever set it. Reachable before this release by resizing the window from
  outside.
- `vid_menudrawfn` and `vid_menukeyfn` were defined in both `menu.c` and
  `vid_x.c`, and only `-fcommon` merged the two into one symbol. `menu.c` owns
  them now.
- **A command line longer than 1023 bytes killed the engine.** `Cbuf_Execute`
  copies each line out of the 8 KB command buffer into a 1024-byte array on the
  stack with `memcpy`, using the length it measured in the buffer and not the
  size of the array, then writes a nul one past that. One command with no
  newline or semicolon in it — a config file whose last line has no terminator,
  or a long enough `bind` — overran it; glibc's `_FORTIFY_SOURCE` check turns
  that into `SIGABRT`, which is why it aborts rather than doing something
  worse. Such a line is now reported and dropped, because half a command is not
  the command that was asked for.
- **`Cbuf_AddText: overflow` now says what overflowed.** The 1996 message was
  that one word: not how large the buffer is, not how much was in use, and not
  what was being added. Whatever fills the buffer is usually still going, so it
  arrived scores of times and pushed anything that might have explained it off
  the top of the console. It reports once, with the size, the amount in use and
  the start of the text that was dropped, counts the rest, and says how many
  were lost when there is room again.
- **The mouse wheel could flood the command buffer.** The wheel handling added
  in this release called `Key_Event` straight from the X event loop rather than
  through the key queue that everything else goes through. `Sys_SendKeyEvents`
  dispatches at most one queue's worth per frame, and that bound is what keeps
  a frame's key events from outgrowing the command buffer — which `Cbuf_Execute`
  drains only once per frame. Bypassing it meant one frame could take an
  unbounded number of notches, each writing its binding into the buffer. All
  four wheel events go through the queue now, as do the two key paths, so there
  is one way in.
- The X11 driver ignored `MappingNotify`. Xlib caches the keyboard mapping when
  the connection opens, and x11vnc types a character the keymap does not have
  by binding it to a spare keycode and putting the keymap back afterwards, so
  those characters arrived as whatever the stale cache said that keycode used
  to mean.
- A size arriving from outside was taken as given, and the renderer's static
  tables are bounded by `MAXWIDTH` and `MAXHEIGHT`. It is clamped now, and the
  aspect ratio is recomputed, which the resize path never did.

- A comment in the entrypoint had the pak search order backwards. It claimed a
  loose file shadows the pak copy of the same name;
  `COM_AddGameDirectory` pushes the directory onto `com_searchpaths` first and
  each pak on top, so the paks win and a loose file is only reached for a name
  no pak holds. The log line now says that, and this was offered as a
  hypothesis for a crash report, so it is worth correcting in public.

## [1.0.1] — 2026-09-17

### Fixed

- A crash in the container said nothing at all. The engine's stdout is a pipe
  under docker, so the C library block-buffered it, and a SIGSEGV during
  startup took the entire log with it — the container printed nothing between
  the entrypoint's "running: xquake" and the shell's "Segmentation fault", so
  the one question worth asking, how far did it get, had no answer. stdout is
  line-buffered now.
- The engine prints a backtrace when it dies on SIGSEGV, SIGBUS, SIGFPE,
  SIGILL or SIGABRT, with the signal, the faulting address and named frames,
  then re-raises so the exit status is unchanged. `-rdynamic` is what makes the
  names available; `strip` keeps the dynamic symbol table, so the shipped
  binary reports them too.
- The startup log now lists anything other than pak files that a mount
  contributed to a game directory. Once the registered game is running the
  engine searches the directory as well as the paks, and a loose file shadows
  the pak copy of the same name, so a mount holding both an extracted tree and
  the paks can feed the engine a mixture — which nothing in the log used to
  show.

## [1.0.0] — 2026-09-17

First release.

The 1999 GPL source release, repaired until it builds and runs on a current
64-bit Linux, and packaged as a container you play in a browser.

### The sources

Repaired, all of them bugs the release always had:

- `sv_main.c` made a QuakeC string offset by subtracting `pr_strings` from a
  pointer in bss. On a 32-bit machine that always fitted in the `int` the field
  is; here the two are terabytes apart, so it truncated, and `world.model`,
  `mapname` and the buffer `ftos`/`vtos`/`etos` return all became wild
  pointers. Nothing faulted at the assignment — the first QuakeC `==` on a
  string did, in `worldspawn`, so no map would load. Whether it crashed at all
  depended on the resolution, and demo playback never touched it, so the engine
  played three demos faultlessly and died the moment anybody started a game.
- `pr_edict.c` sized an `ev_pointer` progs field with `sizeof(void *)/4`, which
  is 2 here where a progs slot is one.
- `model.c` walked `mtexinfo_t::vecs` off the end of its first row, which gcc
  diagnoses as undefined behaviour and may delete — taking the second texture
  axis of every surface in the map with it.
- `r_part.c` had the same bug in the particle field's velocities.
- `vid_x.c` set the framebuffer pointer to the `XImage` header rather than to
  the pixels, so the non-shared-memory path overwrote the structure describing
  where to draw.
- `vid_x.c` typed a 24-bit pixel as `unsigned long`, which is eight bytes on
  LP64 and wrote twice the length of every scanline.
- `vid_x.c` wrote a nul into the string `getenv` returned, emptying `DISPLAY`
  for the process and everything it exec'd.
- `sbar.c` filled a 20-byte scoreboard row from a 32-byte player name, which a
  server chooses.
- `net_udp.c` declared `gethostname` with an `int` length, which disagrees with
  glibc on LP64 and is a hard error.
- `chase.c` called `SV_RecursiveHullCheck` with no declaration in scope.
- `common.h`, `d_surf.c` and `d_edge.c` put pointers and member offsets through
  `int`.
- `common.c` built paths with unbounded `sprintf` into `MAX_OSPATH`, which was
  128.

Modernised where what the code talked to no longer exists:

- `Sys_FloatTime` reads `CLOCK_MONOTONIC` rather than the wall clock.
- The frame loop sleeps out the rest of the frame instead of spinning: 100% of
  a core at the console became 1.9%.
- The default heap went from 8 MB to 64 MB.
- Signals raise a flag that the frame loop acts on, instead of calling
  `Host_Shutdown` and Xlib from the handler.
- `XSynchronize(True)`, left on in the release with "for debugging only" above
  it, is now behind `-verbose`.
- The engine installs its own colormap, because there is no window manager to
  do it.
- Shared memory uses `IPC_PRIVATE` and mode 0600, and its failures are checked.
- The resolution is clamped and rounded; `MAXWIDTH`/`MAXHEIGHT` went from
  1280x1024 to 1920x1200.
- `WM_DELETE_WINDOW` is handled, so closing the window writes the config.

### New

- **Sound**, through a backend that keeps its own clock and writes the mixed
  output to a pipe — `/dev/dsp` has not existed for twenty years and a
  container has no sound card. `audiostream` reads the pipe and serves it to
  the browser alongside the picture, on one port.
- **Music**, from `track02.ogg` and friends, decoded with libsndfile and mixed
  into the engine's own output. Quake's soundtrack was CD audio and was never
  in the game data.
- **The container**: Xvfb at depth 8, x11vnc, noVNC, and a `play.html` that
  captures the mouse — noVNC reports where the pointer is, and a game needs to
  know how far it moved.
- **Controller support**, entirely in the browser, reading the engine's own
  `config.cfg` so that a rebound control still works in a level.
- **A writable game directory**, assembled in the state volume from the
  read-only mount, because Quake writes its config and savegames next to the
  pak files.
- **A smoke test** that builds its own game data, starts the engine on a
  throwaway Xvfb, and checks that it draws a frame and produces a 440 Hz tone —
  there is no game data in this repository to test against. With
  `QUAKE_SMOKE_DATA` pointed at a real install it also loads E1M1, which is
  what found the `pr_strings` bug above; the shareware pak is enough for it.
