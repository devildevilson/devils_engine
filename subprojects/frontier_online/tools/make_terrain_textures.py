# Бесшовные текстуры рельефа для frontier_online: периодический value-noise (решётка с периодом,
# делящим размер), поэтому левый край совпадает с правым, верхний с нижним.
#
#   python3 -I tools/make_terrain_textures.py <каталог>   -> water.png, dry.png (128x128)
#
# Трава взята из имеющихся grass/grass3 и уменьшена с сохранением шва:
#   magick grass.png -virtual-pixel tile -filter Lanczos -resize 128x128 -alpha off -strip out.png
import math, random, struct, sys, zlib

N = 128

def lattice(period, seed):
    rnd = random.Random(seed)
    return [[rnd.random() for _ in range(period)] for _ in range(period)]

def smooth(t):
    return t * t * (3 - 2 * t)

def noise(grid, period, x, y):
    fx = x * period / N; fy = y * period / N
    x0 = int(fx) % period; y0 = int(fy) % period
    x1 = (x0 + 1) % period; y1 = (y0 + 1) % period
    tx = smooth(fx - int(fx)); ty = smooth(fy - int(fy))
    a = grid[y0][x0] + (grid[y0][x1] - grid[y0][x0]) * tx
    b = grid[y1][x0] + (grid[y1][x1] - grid[y1][x0]) * tx
    return a + (b - a) * ty

def fbm(octaves, seed):
    grids = [(p, w, lattice(p, seed + i)) for i, (p, w) in enumerate(octaves)]
    total = sum(w for _, w, _ in grids)
    def f(x, y):
        return sum(w * noise(g, p, x, y) for p, w, g in grids) / total
    return f

def mix(a, b, t):
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))

def write_png(path, pixels):
    raw = b''.join(b'\x00' + bytes(v for px in row for v in px) for row in pixels)
    def chunk(kind, data):
        c = struct.pack('>I', len(data)) + kind + data
        return c + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', N, N, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b'')
    open(path, 'wb').write(png)

def clamp8(v):
    return max(0, min(255, int(round(v))))

def water():
    body = fbm([(4, 1.0), (8, 0.6), (16, 0.35)], 11)
    ripple = fbm([(16, 1.0), (32, 0.7), (64, 0.4)], 23)
    deep, shallow, crest = (28, 74, 132), (44, 104, 160), (120, 170, 205)
    rows = []
    for y in range(N):
        row = []
        for x in range(N):
            c = mix(deep, shallow, body(x, y))
            r = ripple(x, y)
            c = mix(c, crest, max(0.0, (r - 0.62) / 0.38) ** 1.5 * 0.55)
            row.append(tuple(clamp8(v) for v in c))
        rows.append(row)
    return rows

def dry():
    patches = fbm([(4, 1.0), (8, 0.7)], 31)
    grain = fbm([(64, 1.0), (128, 0.8)], 47)
    pebbles = fbm([(32, 1.0)], 59)
    light, dark, stone = (196, 170, 118), (160, 128, 82), (120, 104, 86)
    rows = []
    for y in range(N):
        row = []
        for x in range(N):
            c = mix(light, dark, patches(x, y) * 0.8)
            g = (grain(x, y) - 0.5) * 34
            c = tuple(v + g for v in c)
            p = pebbles(x, y)
            if p > 0.8: c = mix(c, stone, min(1.0, (p - 0.8) / 0.12) * 0.45)
            row.append(tuple(clamp8(v) for v in c))
        rows.append(row)
    return rows

out = sys.argv[1]
write_png(out + '/water.png', water())
write_png(out + '/dry.png', dry())
