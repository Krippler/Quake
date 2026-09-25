#!/usr/bin/env python3
"""Write a .mdl from a pak as an MD5 mesh, animation and .lmp skin.

    python3 tools/mdl2md5.py id1/pak0.pak progs/v_shot.mdl OUTDIR

writes OUTDIR/progs/v_shot.md5mesh, .md5anim and v_shot_00_00.lmp: the layout
the re-release uses for its enhanced models, which the engine reads with
Enhanced Models on (WinQuake/model_md5.c). One joint per vertex, so every
frame survives the trip exactly and the engine should draw the result as it
draws the original. For testing the enhanced-model path without the
re-release's data; the output is only as good as the .mdl it came from.
"""
import struct, sys, os
pak, name, out = sys.argv[1:4]
d = open(pak, 'rb').read()
ofs, ln = struct.unpack_from('<ii', d, 4)
for i in range(ln // 64):
    n = d[ofs+i*64:ofs+i*64+56].split(b'\0')[0].decode()
    fo, fl = struct.unpack_from('<ii', d, ofs+i*64+56)
    if n == name:
        m = d[fo:fo+fl]; break
(ident, ver, sx, sy, sz, ox, oy, oz, rad, ex, ey, ez, nskins, sw, sh, nverts, ntris,
 nframes, sync, flags, size) = struct.unpack_from('<4si3f3ff3f7iif', m, 0)
p = 84
skins = []
for k in range(nskins):
    t, = struct.unpack_from('<i', m, p); p += 4
    assert t == 0
    skins.append(m[p:p+sw*sh]); p += sw*sh
st = [struct.unpack_from('<3i', m, p + 12*i) for i in range(nverts)]; p += 12*nverts
tris = [struct.unpack_from('<4i', m, p + 16*i) for i in range(ntris)]; p += 16*ntris
frames = []
for f in range(nframes):
    t, = struct.unpack_from('<i', m, p); p += 4
    assert t == 0
    p += 8 + 16
    verts = [struct.unpack_from('<4B', m, p + 4*i) for i in range(nverts)]; p += 4*nverts
    frames.append([(v[0]*sx+ox, v[1]*sy+oy, v[2]*sz+oz) for v in verts])
# md5 vertices: (mdl vertex, s) pairs, back-facing seam verts shifted
key = {}; mv = []
def vid(vi, s, t):
    k = (vi, s, t)
    if k not in key:
        key[k] = len(mv); mv.append(k)
    return key[k]
mtris = []
for ff, a, b, c in tris:
    idx = []
    for vi in (a, b, c):
        on, s, t = st[vi]
        if not ff and on: s += sw // 2
        idx.append(vid(vi, s, t))
    mtris.append(idx)
base = os.path.splitext(os.path.basename(name))[0]
os.makedirs(os.path.join(out, 'progs'), exist_ok=True)
with open(os.path.join(out, 'progs', base + '.md5mesh'), 'w') as f:
    f.write('MD5Version 10\ncommandline "mdl2md5"\n\nnumJoints %d\nnumMeshes 1\n\njoints {\n' % nverts)
    for i, v in enumerate(frames[0]):
        f.write('\t"j%d" -1 ( %f %f %f ) ( 0 0 0 )\n' % (i, v[0], v[1], v[2]))
    f.write('}\n\nmesh {\n\tshader "%s"\n\n\tnumverts %d\n' % (base, len(mv)))
    for i, (vi, s, t) in enumerate(mv):
        f.write('\tvert %d ( %f %f ) %d 1\n' % (i, (s + 0.5) / sw, (t + 0.5) / sh, vi))
    f.write('\n\tnumtris %d\n' % len(mtris))
    for i, (a, b, c) in enumerate(mtris):
        f.write('\ttri %d %d %d %d\n' % (i, a, b, c))
    f.write('\n\tnumweights %d\n' % nverts)
    for i in range(nverts):
        f.write('\tweight %d %d 1 ( 0 0 0 )\n' % (i, i))
    f.write('}\n')
with open(os.path.join(out, 'progs', base + '.md5anim'), 'w') as f:
    f.write('MD5Version 10\ncommandline "mdl2md5"\n\nnumFrames %d\nnumJoints %d\nframeRate 10\nnumAnimatedComponents %d\n\nhierarchy {\n' % (nframes, nverts, nverts*3))
    for i in range(nverts):
        f.write('\t"j%d" -1 7 %d\n' % (i, i*3))
    f.write('}\n\nbounds {\n')
    for fr in frames:
        f.write('\t( -1 -1 -1 ) ( 1 1 1 )\n')
    f.write('}\n\nbaseframe {\n')
    for i in range(nverts):
        f.write('\t( 0 0 0 ) ( 0 0 0 )\n')
    f.write('}\n')
    for k, fr in enumerate(frames):
        f.write('\nframe %d {\n' % k)
        for v in fr:
            f.write('\t%f %f %f\n' % v)
        f.write('}\n')
for k, sk in enumerate(skins):
    with open(os.path.join(out, 'progs', '%s_%02d_00.lmp' % (base, k)), 'wb') as f:
        f.write(struct.pack('<ii', sw, sh) + sk)
print(name, nverts, 'verts', ntris, 'tris', nframes, 'frames', len(mv), 'md5 verts', nskins, 'skins', sw, 'x', sh)
