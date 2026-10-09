#!/usr/bin/env python3
"""Render README.md and PORTING.md into the web installer site, so the site always shows the same
documentation as the repository.

    render_docs.py OUT_DIR VERSION

- OUT_DIR/index.html   web/index.html with the README below the install box; its first diagram becomes a
                       gallery of all the README's diagrams
- OUT_DIR/porting.html PORTING.md in the same page shell, without the install box
- OUT_DIR/docs/        the diagrams the README uses

The README's own title and introduction (everything before its first image) are skipped, because the
page has its own header. Links to repository files point to GitHub, PORTING.md points to porting.html,
and heading ids match GitHub's (#tldr, #build-and-flash, ...), so the README's table of contents works.
Uses markdown-it-py (CommonMark, like GitHub) with tables and GitHub-style heading anchors.
"""
import os
import re
import shutil
import sys

from markdown_it import MarkdownIt
from mdit_py_plugins.anchors import anchors_plugin

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO = "https://github.com/romanaerl/M5StickPomodoro"


MD = MarkdownIt("commonmark", {"html": True}).enable("table").enable("strikethrough") \
    .use(anchors_plugin, min_level=1, max_level=4)


def render(md_text):
    html = MD.render(md_text)

    def fix(m):
        attr, url = m.group(1), m.group(2)
        if re.match(r"^(https?:|mailto:|#)", url):
            return m.group(0)
        path, _, frag = url.partition("#")
        if path == "PORTING.md":
            new = "porting.html"
        elif path == "README.md":
            new = "index.html"
        elif path.startswith("docs/") and attr == "src":
            new = path  # copied into the site
        else:
            new = f"{REPO}/blob/main/{path}"
        return f'{attr}="{new}{"#" + frag if frag else ""}"'

    return re.sub(r'(href|src)="([^"]+)"', fix, html)


def gallery(html):
    """Replace the first diagram with a gallery of all the README's diagrams: the selected one large,
    the others as thumbnails beside it. The diagrams stay in their own sections too."""
    imgs = re.findall(r'<p><img src="(docs/[^"]+)" alt="([^"]*)" /></p>', html)
    if len(imgs) < 2:
        return html
    first = f'<p><img src="{imgs[0][0]}" alt="{imgs[0][1]}" /></p>'
    thumbs = "".join(
        f'<button type="button" class="thumb" data-src="{src}" data-alt="{alt}" title="{alt}"'
        f'{" aria-current=\"true\"" if i == 0 else ""}><img src="{src}" alt=""></button>'
        for i, (src, alt) in enumerate(imgs))
    block = (f'<div class="gallery"><figure class="gallery-main"><img src="{imgs[0][0]}" alt="{imgs[0][1]}">'
             f'</figure><div class="gallery-thumbs" role="list">{thumbs}</div></div>')
    return html.replace(first, block, 1)


def page(template, version, docs, with_install):
    html = template.replace("__VERSION__", version).replace("__DOCS__", docs)
    if not with_install:
        html = re.sub(r"<!-- install -->.*?<!-- /install -->", "", html, flags=re.S)
    return html


def main():
    out, version = sys.argv[1], sys.argv[2]
    template = open(os.path.join(ROOT, "web", "index.html"), encoding="utf-8").read()

    readme = open(os.path.join(ROOT, "README.md"), encoding="utf-8").read()
    first_image = readme.find("\n![")
    readme = readme[first_image + 1:] if first_image >= 0 else readme
    with open(os.path.join(out, "index.html"), "w", encoding="utf-8") as f:
        f.write(page(template, version, gallery(render(readme)), True))

    porting = open(os.path.join(ROOT, "PORTING.md"), encoding="utf-8").read()
    porting_html = '<p><a href="index.html">&larr; Back to the installer and guide</a></p>' + render(porting)
    with open(os.path.join(out, "porting.html"), "w", encoding="utf-8") as f:
        f.write(page(template, version, porting_html, False))

    shutil.copytree(os.path.join(ROOT, "docs"), os.path.join(out, "docs"), dirs_exist_ok=True)


if __name__ == "__main__":
    main()
