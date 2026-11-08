# Enclosure

The 3D models of the device's enclosure: the printable files (`.stl`, `.3mf`) and, when there is one, the CAD source (`.step`) they were exported from.

| File | What it is |
|---|---|
| `master-box.stl` | **not in git yet** (29 MB, and the model is not finished; see [todo.md](../todo.md)): kept locally here. The master box, binary STL, 586,152 triangles, Z up; bounding box 68.2 × 108.0 × 68.0 mm (X × Y × Z, connectors and mounting clips included) |
| [`render/master-box-turntable.mp4`](render/master-box-turntable.mp4) | the master box turning one full revolution on a plinth, 8 s, 1920×1080, 30 fps, a seamless loop |
| [`../docs/media/master-box-turntable.webp`](../docs/media/master-box-turntable.webp) | the same, cropped to the box, as the animated WebP the main README shows (1120×800, 30 fps, lossless, 7.5 MB) |

## What is in the STL

One mesh, 14 separate bodies (triangles that share no vertex with each other):

| Bodies | Size (mm) | Part | In the render |
|---|---|---|---|
| 1 | 64 × 108 × 61 | the box | matte black |
| 2 | 10.2 × 19.5 × 11.4 | the two M8 connectors, on the riser, facing +Y | silver; black insert and pins at the open end |
| 2 | 13.3 × 12.0 × 14.1 | the two buttons, on the +X side | matte black |
| 1 | 0.6 × 4.5 × 4.5 | the LED, between the buttons, 0.2 mm behind the face | green, lit |
| 2 | 68.2 × 20.0 × 12.0 | the mounting clips, underneath | matte black |
| 6 | | parts inside the box, not seen | matte black |

<details>
<summary>How the turntable was made</summary>

A [HyperFrames](https://hyperframes.heygen.com) composition (HTML rendered to video): a Three.js scene loads the STL with `STLLoader`, stands it Z up, centres it on a plinth and turns it once over the clip; studio light from a warm key and a cool rim strip, a baked contact shadow, the brand's dark page and the colour wordmark. three.js 0.184.0 and GSAP 3.15.0 are vendored, so rendering fetches nothing.

The parts are coloured from the STL alone, in the browser at load: the mesh is split into bodies (vertices welded at 0.1 µm, then union-find), and each body is recognised by its size in mm (the table above). A connector's insert is every triangle within 3.4 mm of its open end and within 3.6 mm of its axis (the thread is about 4 mm in radius); the rest of it is metal. The LED is emissive, with a faint halo that fades as the LED turns away from the camera. Another STL with differently sized parts would fall back to all black.

The 3D is drawn at twice the frame size and scaled down by the browser (`supersample`, default 2), for clean edges.

Variables: caption, box and button colour, LED colour, supersampling, up axis, turns per clip.

The project is not in git. It is kept in `local/media/turntable-master-box/` without the STL: copy `master-box.stl` into its `assets/` and run `npx hyperframes render -q delivery -o <file>.mp4` there (about 20 s on this PC with the GPU, at 2× supersampling).

The README's WebP is lossless, made from a lossless PNG sequence (not from the MP4); it is cropped to the box (the README has its own logo and caption) and kept at its native 1120px, twice the 560px the README shows it at, for 2× screens:

```
npx hyperframes render --format png-sequence -o frames
ffmpeg -framerate 30 -i frames/frame_%06d.png -vf "crop=1120:800:400:260" -c:v libwebp_anim -lossless 1 -quality 100 -compression_level 6 -loop 0 -an docs/media/master-box-turntable.webp
```

Checked: the bodies and the connector's layers were measured from the STL first (a Node script, sizes and radii along the axis); then frames taken from the MP4 itself (240 frames at 30 fps; the last one leads into the first without a jump), with close-ups of the connectors and the LED. The WebP was compared with the first one (made from the MP4 at quality 82, 880px) at 560px on a 2× screen in headless Chrome: visibly sharper edges and connectors. Lossy at quality 92 (2.6 MB) left blocky steps in the dark gradient behind the box, plain once its dark tones were stretched; lossless has none. The lossless encode takes about 5 minutes.

</details>
