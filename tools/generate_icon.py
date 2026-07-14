"""Generate PickMoji's application icon and MSIX logo assets.

The artwork mirrors the runtime QIcon drawn in app_controller.cpp: a blue
disc (#3390ec) with two white eyes and a white smile arc. Everything is drawn
supersampled and downscaled with LANCZOS so the small icon sizes stay clean.

Usage:
    python tools/generate_icon.py
"""

import os

from PIL import Image, ImageDraw

BLUE = (51, 144, 236, 255)      # #3390ec, the in-app accent
WHITE = (255, 255, 255, 255)
TILE_BG = (23, 33, 43, 255)     # #17212b, the picker panel colour
REF = 64.0                      # reference canvas used by createAppIcon()


def render(size, bg=None, supersample=4):
    """Render the smiley at `size` px, optionally on a solid background."""
    big = size * supersample
    scale = big / REF
    image = Image.new("RGBA", (big, big), bg if bg else (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)

    # Face: ellipse (3,3)-(61,61) in the 64 px reference space.
    draw.ellipse([3 * scale, 3 * scale, 61 * scale, 61 * scale], fill=BLUE)

    # Eyes at (23,26) and (41,26).
    eye_r = 3.0 * scale
    for ex, ey in ((23, 26), (41, 26)):
        draw.ellipse(
            [ex * scale - eye_r, ey * scale - eye_r,
             ex * scale + eye_r, ey * scale + eye_r],
            fill=WHITE,
        )

    # Smile: bottom arc of the box (17,22)-(47,47). PIL angles run clockwise
    # from 3 o'clock, so 25 deg -> 155 deg sweeps across the bottom.
    line_w = max(1, round(4.6 * scale))
    draw.arc([17 * scale, 22 * scale, 47 * scale, 47 * scale],
             start=25, end=155, fill=WHITE, width=line_w)

    if supersample != 1:
        image = image.resize((size, size), Image.LANCZOS)
    return image


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    assets_dir = os.path.join(root, "assets")
    msix_dir = os.path.join(root, "packaging", "msix", "Assets")
    os.makedirs(assets_dir, exist_ok=True)
    os.makedirs(msix_dir, exist_ok=True)

    # Multi-resolution .ico for the executable / Explorer.
    ico_sizes = [16, 24, 32, 48, 64, 128, 256]
    base = render(256)
    ico_path = os.path.join(assets_dir, "PickMoji.ico")
    base.save(ico_path, format="ICO", sizes=[(s, s) for s in ico_sizes])
    print("wrote", ico_path)

    # MSIX square logos (transparent for the small ones, tinted tiles for big).
    square_logos = {
        "StoreLogo.png": (50, None),
        "Square44x44Logo.png": (44, None),
        "Square150x150Logo.png": (150, TILE_BG),
        "Square310x310Logo.png": (310, TILE_BG),
    }
    for name, (size, bg) in square_logos.items():
        path = os.path.join(msix_dir, name)
        render(size, bg=bg).save(path, format="PNG")
        print("wrote", path)

    # Wide tile: logo centred on the branded background.
    wide = Image.new("RGBA", (310, 150), TILE_BG)
    logo = render(120)
    wide.alpha_composite(logo, ((310 - 120) // 2, (150 - 120) // 2))
    wide_path = os.path.join(msix_dir, "Wide310x150Logo.png")
    wide.save(wide_path, format="PNG")
    print("wrote", wide_path)


if __name__ == "__main__":
    main()
