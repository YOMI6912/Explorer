from PIL import Image
from pathlib import Path
import os

IMAGE_FILE = Path(__file__).resolve().parent / "bear.png"

WIDTH = 70
MESSAGE = "goodnight na ifahhhhhhh"

CODE_TEXT = (
    "from_PIL_import_Image;"
    "def_goodnight():"
    "print('goodnight_na_ifahhhhhhh');"
    "for_i_in_range(999):dream+=1;"
    "if_night:print('sweet_dreams');"
)


def show_code_bear(image_path):
    if not image_path.exists():
        print(f"ไม่พบไฟล์รูป: {image_path}")
        print("ให้นำรูปหมีชื่อ bear.png มาวางข้างไฟล์ loopy.py")
        return

    image = Image.open(image_path).convert("RGBA")

    # ทำพื้นหลังโปร่งใสให้เป็นสีดำ
    background = Image.new("RGBA", image.size, (0, 0, 0, 255))
    image = Image.alpha_composite(background, image).convert("RGB")

    # ปรับสัดส่วนให้เหมาะกับตัวอักษรใน Terminal
    height = int((image.height / image.width) * WIDTH * 0.50)

    image = image.resize(
        (WIDTH, height),
        Image.Resampling.LANCZOS
    )

    code_index = 0

    for y in range(height):
        # กำหนดพื้นหลังบรรทัดเป็นสีดำ
        row = "\033[48;2;0;0;0m"

        for x in range(WIDTH):
            r, g, b = image.getpixel((x, y))

            # พื้นหลังสีดำให้แสดงเป็นช่องว่าง
            if max(r, g, b) < 25:
                row += " "
            else:
                character = CODE_TEXT[code_index % len(CODE_TEXT)]
                code_index += 1

                # ใช้สีจริงจากรูปกับตัวอักษรแต่ละตัว
                row += f"\033[38;2;{r};{g};{b}m{character}"

        row += "\033[0m"
        print(row)


# เปิดใช้งานสี ANSI บน Windows
if os.name == "nt":
    os.system("")

os.system("cls")

show_code_bear(IMAGE_FILE)

print()
print(
    f"\033[1;38;2;255;90;200m"
    f"{MESSAGE.center(WIDTH)}"
    f"\033[0m"
)
print()