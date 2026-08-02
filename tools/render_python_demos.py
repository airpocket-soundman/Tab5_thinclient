"""Run the bundled gfx demos against a recorder and build the site demo movie.

The demo files are executed directly.  ``RecorderGfx`` implements the same
drawing surface exposed by the firmware, so the captured frames come from the
actual demo programs rather than from hand-authored approximations.

Dependencies: Pillow, NumPy, imageio, and imageio-ffmpeg.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path

import imageio.v2 as imageio
import numpy as np
from PIL import Image, ImageDraw, ImageFont


DISPLAY_W = 1280
DISPLAY_H = 720
HEADER_H = 44
CANVAS_H = DISPLAY_H - HEADER_H
BG = (11, 14, 17)
HEADER = (42, 47, 52)
GREEN = (48, 255, 112)
CYAN = (53, 213, 229)
MUTED = (139, 152, 165)


def font(size: int, bold: bool = False) -> ImageFont.FreeTypeFont | ImageFont.ImageFont:
    candidates = [
        Path("C:/Windows/Fonts/consolab.ttf" if bold else "C:/Windows/Fonts/consola.ttf"),
        Path("/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf" if bold else "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"),
    ]
    for candidate in candidates:
        if candidate.exists():
            return ImageFont.truetype(str(candidate), size)
    return ImageFont.load_default()


FONT_16 = font(16)
FONT_20 = font(20)
FONT_24 = font(24, bold=True)
FONT_34 = font(34, bold=True)


def rgb(value: int) -> tuple[int, int, int]:
    return ((value >> 16) & 0xFF, (value >> 8) & 0xFF, value & 0xFF)


class RecorderGfx:
    def __init__(self, capture_every: int = 1) -> None:
        self.image = Image.new("RGB", (DISPLAY_W, CANVAS_H), "black")
        self.draw = ImageDraw.Draw(self.image)
        self.capture_every = max(1, capture_every)
        self.present_count = 0
        self.captured: list[Image.Image] = []

    def size(self) -> tuple[int, int]:
        return DISPLAY_W, CANVAS_H

    def clear(self, color: int = 0) -> None:
        self.draw.rectangle((0, 0, DISPLAY_W - 1, CANVAS_H - 1), fill=rgb(color))

    def pixel(self, x: int, y: int, color: int) -> None:
        if 0 <= x < DISPLAY_W and 0 <= y < CANVAS_H:
            self.draw.point((x, y), fill=rgb(color))

    def line(self, x0: int, y0: int, x1: int, y1: int, color: int) -> None:
        self.draw.line((x0, y0, x1, y1), fill=rgb(color), width=1)

    def rect(self, x: int, y: int, w: int, h: int, color: int, fill: int = 1) -> None:
        if w <= 0 or h <= 0:
            return
        box = (x, y, x + w - 1, y + h - 1)
        if fill:
            self.draw.rectangle(box, fill=rgb(color))
        else:
            self.draw.rectangle(box, outline=rgb(color), width=1)

    def fill(self, x: int, y: int, w: int, h: int, color: int) -> None:
        self.rect(x, y, w, h, color, 1)

    def circle(self, x: int, y: int, radius: int, color: int, fill: int = 1) -> None:
        box = (x - radius, y - radius, x + radius, y + radius)
        if fill:
            self.draw.ellipse(box, fill=rgb(color))
        else:
            self.draw.ellipse(box, outline=rgb(color), width=1)

    def text(self, x: int, y: int, value: str, color: int = 0xFFFFFF) -> None:
        # The firmware uses an opaque 8x16 ASCII font on a black background.
        width = max(1, len(value) * 10)
        self.draw.rectangle((x, y, x + width, y + 18), fill="black")
        self.draw.text((x, y), value, fill=rgb(color), font=FONT_16)

    def mono(
        self,
        x0: int,
        y0: int,
        cols: int,
        rows: int,
        cell: int,
        foreground: int,
        background: int,
        bits: str,
    ) -> None:
        for y in range(rows):
            for x in range(cols):
                index = y * cols + x
                nibble = int(bits[index // 4], 16)
                enabled = bool(nibble & (1 << (3 - index % 4)))
                color = foreground if enabled else background
                self.rect(x0 + x * cell + 1, y0 + y * cell + 1, max(1, cell - 2), max(1, cell - 2), color, 1)

    def present(self) -> None:
        self.present_count += 1
        if self.present_count == 1 or self.present_count % self.capture_every == 0:
            self.captured.append(self.image.copy())

    def final_frame(self) -> Image.Image:
        return self.image.copy()


@dataclass(frozen=True)
class Demo:
    script: str
    title: str
    subtitle: str
    argv: tuple[str, ...]
    capture_every: int


DEMOS = (
    Demo("mandel.py", "PROGRESSIVE MANDELBROT", "coarse blocks resolve into the fractal", ("0", "1", "8", "0"), 6),
    Demo("plasma.py", "SINE PLASMA", "four wave fields mapped into RGB", ("0", "60", "24"), 2),
    Demo("hat.py", "WIREFRAME HAT", "row-by-row sombrero surface rendering", ("0", "1", "0"), 2),
    Demo("life.py", "GAME OF LIFE", "25 x 25 bitmap generations", ("0", "90", "10"), 3),
    Demo("starfield.py", "STARFIELD", "deterministic center-out flight", ("0", "70", "7"), 2),
    Demo("maze.py", "MAZE", "depth-first generation and path finding", ("0", "25", "17"), 10),
)


def full_display(canvas: Image.Image, title: str) -> Image.Image:
    frame = Image.new("RGB", (DISPLAY_W, DISPLAY_H), BG)
    frame.paste(canvas, (0, HEADER_H))
    draw = ImageDraw.Draw(frame)
    draw.rectangle((0, 0, DISPLAY_W, HEADER_H - 1), fill=HEADER)
    draw.text((14, 11), "TERMINAL", fill=GREEN, font=FONT_16)
    draw.text((132, 11), "WIFI", fill=MUTED, font=FONT_16)
    draw.text((194, 11), "SSH", fill=MUTED, font=FONT_16)
    draw.text((DISPLAY_W - 330, 11), "MICROPYTHON / " + title, fill=CYAN, font=FONT_16)
    return frame


def title_card(demo: Demo, command: str) -> Image.Image:
    image = Image.new("RGB", (DISPLAY_W, DISPLAY_H), BG)
    draw = ImageDraw.Draw(image)
    draw.rectangle((0, 0, 10, DISPLAY_H), fill=GREEN)
    draw.text((76, 238), demo.title, fill="white", font=FONT_34)
    draw.text((78, 296), demo.subtitle, fill=MUTED, font=FONT_20)
    draw.text((78, 360), "tab5:/$ " + command, fill=GREEN, font=FONT_20)
    return image


def run_demo(demos_dir: Path, demo: Demo) -> tuple[list[Image.Image], Image.Image, str]:
    source = demos_dir / demo.script
    command = "python /" + demo.script + " " + " ".join(demo.argv)
    recorder = RecorderGfx(demo.capture_every)
    namespace = {
        "__name__": "__main__",
        "__file__": str(source),
        "gfx": recorder,
        "argv": [demo.script, *demo.argv],
    }
    exec(compile(source.read_text(encoding="utf-8"), str(source), "exec"), namespace)
    final = recorder.final_frame()
    frames = recorder.captured
    if not frames or frames[-1].tobytes() != final.tobytes():
        frames.append(final)
    return frames, final, command


def poster(representatives: list[tuple[Demo, Image.Image]], output: Path) -> None:
    thumb_w, thumb_h = 600, 338
    canvas = Image.new("RGB", (thumb_w * 2, thumb_h * 3), BG)
    draw = ImageDraw.Draw(canvas)
    for index, (demo, frame) in enumerate(representatives):
        x = (index % 2) * thumb_w
        y = (index // 2) * thumb_h
        tile = full_display(frame, demo.title).resize((thumb_w, thumb_h), Image.Resampling.LANCZOS)
        canvas.paste(tile, (x, y))
        draw.rectangle((x, y + thumb_h - 38, x + thumb_w, y + thumb_h), fill=(0, 0, 0))
        draw.text((x + 15, y + thumb_h - 30), demo.title, fill="white", font=FONT_20)
    output.parent.mkdir(parents=True, exist_ok=True)
    canvas.save(output, quality=90, optimize=True)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--fps", type=int, default=15)
    parser.add_argument("--output", type=Path, default=Path("site/media/python-demos.mp4"))
    parser.add_argument("--poster", type=Path, default=Path("site/images/python-demos-poster.jpg"))
    args = parser.parse_args()

    root = Path(__file__).resolve().parents[1]
    demos_dir = root / "demos"
    output = (root / args.output).resolve() if not args.output.is_absolute() else args.output
    poster_output = (root / args.poster).resolve() if not args.poster.is_absolute() else args.poster
    output.parent.mkdir(parents=True, exist_ok=True)

    representatives: list[tuple[Demo, Image.Image]] = []
    with imageio.get_writer(
        output,
        fps=args.fps,
        codec="libx264",
        pixelformat="yuv420p",
        quality=7,
        macro_block_size=None,
        ffmpeg_log_level="warning",
    ) as writer:
        for demo in DEMOS:
            frames, final, command = run_demo(demos_dir, demo)
            card = np.asarray(title_card(demo, command))
            for _ in range(max(1, args.fps * 2 // 3)):
                writer.append_data(card)
            for frame in frames:
                writer.append_data(np.asarray(full_display(frame, demo.title)))
            ending = np.asarray(full_display(final, demo.title))
            for _ in range(max(1, args.fps // 2)):
                writer.append_data(ending)
            representatives.append((demo, final))
            print(f"{demo.script}: {len(frames)} captured frames")

    poster(representatives, poster_output)
    print(f"video:  {output}")
    print(f"poster: {poster_output}")


if __name__ == "__main__":
    main()
