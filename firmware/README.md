
## Build

### Fetch Dependencies

```bash
python3 ./fetch_repos.py
```

### Tool Chains

[ESP-IDF v6.1](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32s3/index.html)

### Build

```bash
idf.py build
```

### Host-side tests

The motion coordinate helpers can be tested without ESP-IDF hardware:

```bash
cmake -S tests -B build-host-tests
cmake --build build-host-tests
ctest --test-dir build-host-tests --output-on-failure
```

### Flash

```bash
idf.py flash
```

## Customization

### Custom faces

Draw your own face as layered PNGs and install it with one command -- no C++.
The firmware composes all 21 assistant emotions from a handful of eyelid shapes
and mouth curves, and animates them with the existing blink, lip sync, breathing
and gaze systems. Falls back to the stock face if none is installed.

```bash
pip install PyYAML Pillow pypng lz4

python tools/face.py init my_face        # starter face.yaml
python tools/face.py preview my_face     # render all 21 emotions, no hardware
python tools/face.py build my_face       # convert + install
idf.py build && idf.py flash
```

`preview` composites your artwork exactly the way the device layers it, so the
draw-check-redraw loop takes a second instead of a reflash.

Full guide: [docs/custom-faces.md](docs/custom-faces.md)
