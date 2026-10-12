# ytm-lite のアイコン(src/ytm-lite.ico)を作る。Pillow が要る。
#   python tools/make_icon.py
# 赤い角丸の四角に白い八分音符。大きく描いてから各サイズへ縮める。
from PIL import Image, ImageDraw
import os

S = 1024


def draw():
    im = Image.new("RGBA", (S, S), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((40, 40, S - 40, S - 40), radius=210, fill=(220, 30, 40, 255))
    white = (255, 255, 255, 255)
    # 符頭(少し傾いた楕円の代わりに円)
    d.ellipse((250, 600, 520, 840), fill=white)
    # 符幹
    d.rectangle((455, 220, 525, 730), fill=white)
    # 旗
    d.polygon([(525, 220), (790, 330), (790, 440), (525, 340)], fill=white)
    return im


def main():
    big = draw()
    sizes = [16, 20, 24, 32, 40, 48, 64, 256]
    out = os.path.join(os.path.dirname(__file__), "..", "src", "ytm-lite.ico")
    big.save(out, format="ICO", sizes=[(s, s) for s in sizes])
    print("wrote", os.path.normpath(out))


if __name__ == "__main__":
    main()
