# Wallpaper gallery — 31 October 2026

The published build-293 Mesa ISO was downloaded and booted directly in QEMU.
All three landscape thumbnails were present, mouse selection succeeded and the
64-bit decoder loaded each 3200x1800 image. Missing packaged JPEGs were not
reproduced in that artifact.

The previous Settings grid drew rows until its lower bound and discarded the
rest, with no navigation to reach clipped items. Layout also placed personal
pictures and position controls after all preset rows, which can put controls
outside a smaller window. The three new landscapes were at the end of the grid.

## Change

The gallery now places photographic landscapes first and displays the total
preset count. Its capacity reserves space for personal pictures and controls.
When all presets do not fit, Previous/Next and the mouse wheel change pages.
Drawing, keyboard targets and mouse hits use the same visible page. Position
controls occupy a separate row so they fit the minimum window width.

An empty personal-picture history uses less vertical space. Footer controls
that cannot fit exceptionally short windows are omitted from both drawing and
hit targets; the gallery retains a navigable row.

The original preset IDs and stored names remain stable. The visual order is a
mapping to those IDs, preserving existing CLI numbers and wallpaper choices.

## Validation

- The actual wallpaper-manager host test now checks the order, unique coverage
  of all 13 presets and page layout across content widths 260–1000 and heights
  220–700, with empty and nonempty history. ASan/UBSan pass on i686 and x86_64,
  alongside the existing I/O, configuration, dimension and image-cache cases.
- Both kernels compile. Native 64-bit GUI mouse clicks select all three
  landscapes from the first row and CLI readback confirms each choice.
- A 580x470 Settings window with five synthetic history entries exercises
  paging: Next reaches Plain Black and Previous returns to Alpine Peaks.
  These history fixtures exercise layout, not personal-image preview fidelity.
- Native 32-bit GUI screenshots verify all three first-row thumbnails and
  their selection borders after mouse clicks. That architecture retains its
  existing thumbnail rendering fallback.
- A normal-window preview is assets/screenshots/wallpaper-gallery.png.

Local logs/screenshots live under
/home/banana/bos-tests/wallpaper-release-20261031/. Local ISOs are validation
inputs; GitHub Actions publishes the distributable images after the source push.
