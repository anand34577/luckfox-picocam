#!/usr/bin/env python3
"""Builds the GitHub Wiki pages from the Markdown in this repository.

The wiki is generated, not hand-edited: edit docs/*.md and README.md, push to main, and the Wiki workflow publishes them.

  python3 .github/scripts/build-wiki.py OUTPUT_DIR
"""
import os
import re
import sys

REPO = os.environ.get("GITHUB_REPOSITORY", "anand34577/luckfox-picocam")
BRANCH = "main"
RAW = f"https://raw.githubusercontent.com/{REPO}/{BRANCH}"
BLOB = f"https://github.com/{REPO}/blob/{BRANCH}"
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))

# source file -> wiki page name
PAGES = {
    "README.md": "Home",
    "docs/INSTALL.md": "Install-Guide",
    "docs/USER-GUIDE.md": "User-Guide",
    "docs/CONNECTING.md": "Connecting-Other-Software",
    "docs/MODELS.md": "AI-Models",
    "docs/BOARDS.md": "Supported-Boards",
    "docs/FAQ.md": "FAQ",
    "docs/TROUBLESHOOTING.md": "Troubleshooting",
    "docs/BUILDING.md": "Building-from-Source",
    "CONTRIBUTING.md": "Contributing",
    "CHANGELOG.md": "Changelog",
}
BY_BASENAME = {os.path.basename(k): v for k, v in PAGES.items()}

SIDEBAR = """**PicoCam**

* [Home](Home)

**Getting started**

* [Install Guide](Install-Guide)
* [User Guide](User-Guide)
* [FAQ](FAQ)
* [Troubleshooting](Troubleshooting)

**Going further**

* [Connecting Other Software](Connecting-Other-Software)
* [AI Models](AI-Models)
* [Supported Boards](Supported-Boards)

**Project**

* [Building from Source](Building-from-Source)
* [Contributing](Contributing)
* [Changelog](Changelog)
* [Releases](https://github.com/%s/releases)
""" % REPO

FOOTER = "This wiki is generated from the files in the repository (`docs/`). To fix a page, send a pull request against the matching file.\n"


def convert(src, text):
    base = os.path.dirname(src)

    def link(m):
        label, target = m.group(1), m.group(2)
        if re.match(r"^(https?:|mailto:|#)", target):
            return m.group(0)
        path, _, frag = target.partition("#")
        frag = "#" + frag if frag else ""
        full = os.path.normpath(os.path.join(base, path)).replace("\\", "/")
        name = os.path.basename(full)
        if path.endswith(".md") and name in BY_BASENAME:
            return f"[{label}]({BY_BASENAME[name]}{frag})"
        if full.startswith("docs/images/") or re.search(r"\.(png|jpg|svg|gif)$", full):
            return f"[{label}]({RAW}/{full})"
        return f"[{label}]({BLOB}/{full}{frag})"

    # links and images: ![alt](path) / [label](path)
    def img(m):
        alt, target = m.group(1), m.group(2)
        if re.match(r"^https?:", target):
            return m.group(0)
        full = os.path.normpath(os.path.join(base, target)).replace("\\", "/")
        return f"![{alt}]({RAW}/{full})"

    text = re.sub(r"!\[([^\]]*)\]\(([^)\s]+)\)", img, text)
    text = re.sub(r'<img ([^>]*)src="([^"]+)"', lambda m: m.group(0) if m.group(2).startswith("http")
                  else f'<img {m.group(1)}src="{RAW}/{os.path.normpath(os.path.join(base, m.group(2))).replace(chr(92), "/")}"', text)
    text = re.sub(r"(?<!!)\[([^\]]+)\]\(([^)\s]+)\)", link, text)
    return text


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "wiki-out"
    os.makedirs(out, exist_ok=True)
    for src, name in PAGES.items():
        with open(os.path.join(ROOT, src), encoding="utf-8") as f:
            text = f.read()
        if name == "Home":
            text = re.sub(r'<p align="center">\s*<img src="docs/images/banner.png"[^>]*>\s*</p>\n*', "", text, count=1)
        with open(os.path.join(out, name + ".md"), "w", encoding="utf-8", newline="\n") as f:
            f.write(convert(src, text))
    with open(os.path.join(out, "_Sidebar.md"), "w", encoding="utf-8", newline="\n") as f:
        f.write(SIDEBAR)
    with open(os.path.join(out, "_Footer.md"), "w", encoding="utf-8", newline="\n") as f:
        f.write(FOOTER)
    print(f"wrote {len(PAGES) + 2} wiki files to {out}")


if __name__ == "__main__":
    main()
