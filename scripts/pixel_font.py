"""Render the firmware's ASCII u8g2 font into Pillow previews at native pixels.

Uses the upstream font data and its documented LSB-first glyph RLE format.
Coordinates use the same baseline and string-width rules as canvas_draw_str.
"""
import ast
import re


class PixelFont:
    def __init__(self, source, name):
        text = source.read_text(encoding='latin-1')
        body = re.search(r'const uint8_t ' + re.escape(name) +
                         r'\[\d+\][^=]*=\s*((?:"(?:\\.|[^"\\])*"\s*)+);', text)[1]
        self.data = b''.join(ast.literal_eval('b' + literal) for literal in
                             re.findall(r'"(?:\\.|[^"\\])*"', body))
        self.glyphs = {}
        pos = 23
        while self.data[pos + 1]:
            code, length = self.data[pos:pos + 2]
            self.glyphs[chr(code)] = self._decode(self.data[pos + 2:pos + length])
            pos += length
        self.ascent = self.data[13]

    def _decode(self, data):
        bit = 0

        def read(count, signed=False):
            nonlocal bit
            value = sum(((data[(bit + n) // 8] >> ((bit + n) % 8)) & 1) << n
                        for n in range(count))
            bit += count
            return value - (1 << (count - 1)) if signed else value

        header = self.data
        width, height = read(header[4]), read(header[5])
        x, y, advance = (read(header[n], True) for n in (6, 7, 8))
        pixels = []
        cursor = 0
        while cursor < width * height:
            clear, ink = read(header[2]), read(header[3])
            while True:
                cursor += clear
                pixels.extend((n % width, n // width) for n in range(cursor, cursor + ink))
                cursor += ink
                if not read(1):
                    break
        return width, height, x, y, advance, pixels

    def width(self, text):
        if not text:
            return 0
        glyphs = [self.glyphs[c] for c in text]
        result = sum(g[4] for g in glyphs)
        if glyphs[-1][0]:
            result += glyphs[-1][0] + glyphs[-1][2] - glyphs[-1][4]
        return result

    def draw(self, image, x, baseline, text, align='left'):
        width = self.width(text)
        if align == 'right':
            x -= width
        elif align == 'center':
            x -= width // 2
        for char in text:
            _, height, dx, dy, advance, pixels = self.glyphs[char]
            for px, py in pixels:
                point = x + dx + px, baseline - height - dy + py
                if not (0 <= point[0] < image.width and 0 <= point[1] < image.height):
                    raise ValueError(f'Font pixel outside preview: {text!r} at {point}')
                image.putpixel(point, 0)
            x += advance
