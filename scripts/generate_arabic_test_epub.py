#!/usr/bin/env python3
"""
Generate a test EPUB with Arabic body text to verify Arabic reading support.

Exercises:
- Long Arabic paragraphs (line-wrapping / word-boundary shaping across many lines)
- Bold (<strong>) Arabic text (known gap: no bold Arabic glyphs today, so this
  is expected to render invisible until that's fixed)
- Numbers mixed into Arabic sentences (classic bidi torture-test case: LTR
  digits embedded in an RTL paragraph)
- dc:language set to "ar" (confirms hyphenation gracefully no-ops for a
  language with no registered hyphenation pattern)

All Arabic text below is original placeholder prose written for this test,
not reproduced from any external source.
"""

import sys
import zipfile
from pathlib import Path

if any(arg in ("-h", "--help") for arg in sys.argv[1:]):
    print(__doc__.strip())
    sys.exit(0)

OUTPUT_DIR = Path(__file__).parent.parent / "test" / "epubs"


def create_epub(epub_path, title, language, chapters):
    """
    chapters: list of (chapter_title, html_content)
    """
    with zipfile.ZipFile(epub_path, "w", zipfile.ZIP_DEFLATED) as epub:
        epub.writestr("mimetype", "application/epub+zip", compress_type=zipfile.ZIP_STORED)

        container_xml = """<?xml version="1.0" encoding="UTF-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>"""
        epub.writestr("META-INF/container.xml", container_xml)

        manifest_items = []
        spine_items = []

        for i, (chapter_title, html_content) in enumerate(chapters):
            chapter_id = f"chapter{i + 1}"
            chapter_file = f"chapter{i + 1}.xhtml"
            manifest_items.append(
                f'    <item id="{chapter_id}" href="{chapter_file}" media-type="application/xhtml+xml"/>'
            )
            spine_items.append(f'    <itemref idref="{chapter_id}"/>')
            epub.writestr(f"OEBPS/{chapter_file}", html_content)

        content_opf = f"""<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="uid">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="uid">test-epub-{title.lower().replace(" ", "-")}</dc:identifier>
    <dc:title>{title}</dc:title>
    <dc:language>{language}</dc:language>
  </metadata>
  <manifest>
    <item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>
{chr(10).join(manifest_items)}
  </manifest>
  <spine>
{chr(10).join(spine_items)}
  </spine>
</package>"""
        epub.writestr("OEBPS/content.opf", content_opf)

        nav_items = "\n".join(
            f'      <li><a href="chapter{i + 1}.xhtml">{chapters[i][0]}</a></li>' for i in range(len(chapters))
        )
        nav_xhtml = f"""<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops">
<head><title>Navigation</title></head>
<body>
  <nav epub:type="toc">
    <h1>Contents</h1>
    <ol>
{nav_items}
    </ol>
  </nav>
</body>
</html>"""
        epub.writestr("OEBPS/nav.xhtml", nav_xhtml)


def make_chapter(title, body_content):
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" dir="rtl" lang="ar">
<head><title>{title}</title></head>
<body dir="rtl">
<h1>{title}</h1>
{body_content}
</body>
</html>"""


def main():
    OUTPUT_DIR.mkdir(exist_ok=True)

    chapters = [
        (
            "مقدمة",
            make_chapter(
                "مقدمة",
                """
<p>هذا كتاب اختباري باللغة العربية. الهدف منه هو التأكد من أن النص العربي
يظهر بشكل صحيح على الجهاز، مع التفاف الأسطر في الأماكن الصحيحة بين الكلمات.</p>
<p>يجب أن تكون الحروف متصلة ببعضها بالشكل الصحيح حسب موقع كل حرف في الكلمة،
سواء في البداية أو الوسط أو النهاية أو عندما يكون الحرف منعزلاً.</p>
""",
            ),
        ),
        (
            "نص عريض",
            make_chapter(
                "نص عريض",
                """
<p>هذه فقرة عادية بدون أي تنسيق خاص، تليها فقرة تحتوي على نص عريض.</p>
<p>وهذا مثال على <strong>نص عريض باللغة العربية</strong> يظهر داخل فقرة عادية،
لاختبار ما إذا كانت الحروف الغامقة تظهر بشكل صحيح أو تختفي تماماً.</p>
<p>وهنا أيضاً بعض الكلمات <b>بخط عريض</b> باستخدام وسم آخر لنفس الغرض.</p>
""",
            ),
        ),
        (
            "أرقام مختلطة",
            make_chapter(
                "أرقام مختلطة",
                """
<p>تم نشر هذا الكتاب في عام 2024 ميلادي، وقد طبعت منه 1500 نسخة في الإصدار
الأول، ثم أضيفت 320 نسخة أخرى لاحقاً ليصبح المجموع 1820 نسخة.</p>
<p>الفصل رقم 5 يبدأ في الصفحة 102 وينتهي في الصفحة 118، أي أنه يمتد على مدى
16 صفحة تقريباً، وهذا يعادل نحو 7% من إجمالي عدد صفحات الكتاب البالغ 240 صفحة.</p>
<p>في عام 1990 كان عدد السكان 45000 نسمة، وارتفع بحلول عام 2020 إلى 128000 نسمة،
أي بزيادة قدرها 83000 نسمة خلال 30 عاماً.</p>
""",
            ),
        ),
        (
            "فقرات طويلة",
            make_chapter(
                "فقرات طويلة",
                """
<p>في قديم الزمان، وفي أرض بعيدة تحيط بها الجبال الشاهقة والوديان الخضراء،
كانت هناك قرية صغيرة يعيش فيها أناس طيبون يعملون في الزراعة وتربية المواشي،
وكانوا يجتمعون كل مساء تحت شجرة كبيرة يتبادلون فيها الأحاديث والحكايات التي
توارثوها عن أجدادهم منذ عشرات السنين، وكان كبير القرية يروي لهم كل ليلة قصة
جديدة عن الشجاعة والكرم والصدق، وكان الأطفال يستمعون بشغف كبير وهم يحلمون
بأن يكونوا يوماً ما أبطالاً في قصص مشابهة يرويها أحفادهم من بعدهم.</p>
<p>ومع مرور الأيام والسنين، تغيرت أحوال القرية كثيراً، فقد بُنيت الطرق
الحديثة، ووصلت الكهرباء إلى كل بيت، وأصبح الشباب يسافرون إلى المدن الكبيرة
طلباً للعلم والعمل، لكنهم كانوا يعودون كل صيف ليجلسوا من جديد تحت تلك الشجرة
العتيقة، ويستمعوا إلى الحكايات القديمة نفسها التي لم تتغير رغم تغير كل شيء
من حولهم، وكأن تلك الحكايات كانت الخيط الوحيد الذي يربطهم بجذورهم الأولى.</p>
""",
            ),
        ),
    ]

    epub_path = OUTPUT_DIR / "test_arabic_text.epub"
    create_epub(epub_path, "Arabic Text Test", "ar", chapters)
    print(f"Created: {epub_path}")


if __name__ == "__main__":
    main()
