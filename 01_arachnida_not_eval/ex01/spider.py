#!/usr/bin/env python3
"""
spider - recursively download images from a website.

Usage:
    ./spider.py [-r] [-l N] [-p PATH] URL

Options:
    -r          download images recursively by following links
    -l N        maximum recursion depth (default 5, only meaningful with -r)
    -p PATH     directory where images are saved (default ./data/)

Allowed by the subject: libraries for HTTP requests and file handling.
The crawling logic (parsing, URL resolution, recursion, dedup) is hand-written.
Only the Python standard library is used, so there is nothing to install.
"""

import argparse
import os
import sys
from html.parser import HTMLParser
from urllib.parse import urljoin, urlparse, urldefrag
from urllib.request import Request, urlopen
from urllib.error import URLError, HTTPError

IMAGE_EXTENSIONS = (".jpg", ".jpeg", ".png", ".gif", ".bmp")
USER_AGENT = "arachnida-spider/1.0"
TIMEOUT = 10


class LinkImageParser(HTMLParser):
    """Collect image sources and anchor links from an HTML page.

    We subclass the stdlib parser and write the tag-handling logic
    ourselves, so the HTML interpretation is our own code.
    """

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.images = []  # raw (possibly relative) image URLs
        self.links = []   # raw (possibly relative) anchor URLs

    def _add_srcset(self, value):
        # srcset = "a.jpg 1x, b.jpg 2x" -> keep the URL part of each entry
        for part in value.split(","):
            url = part.strip().split(" ")[0].strip()
            if url:
                self.images.append(url)

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if tag == "img":
            if attrs.get("src"):
                self.images.append(attrs["src"])
            if attrs.get("srcset"):
                self._add_srcset(attrs["srcset"])
        elif tag == "source":  # <picture>/<source>
            if attrs.get("src"):
                self.images.append(attrs["src"])
            if attrs.get("srcset"):
                self._add_srcset(attrs["srcset"])
        elif tag == "a" and attrs.get("href"):
            # anchors feed both recursion and direct-to-image links
            self.links.append(attrs["href"])
        elif tag == "link" and attrs.get("href"):
            # <link rel="icon"/"apple-touch-icon"> favicons etc.; non-image
            # hrefs (stylesheets) are filtered out later by is_image_url
            self.images.append(attrs["href"])


def is_image_url(url):
    path = urlparse(url).path.lower()
    return path.endswith(IMAGE_EXTENSIONS)


def fetch(url):
    """Return (content_bytes, is_html). Raises on network error."""
    req = Request(url, headers={"User-Agent": USER_AGENT})
    with urlopen(req, timeout=TIMEOUT) as resp:
        ctype = resp.headers.get("Content-Type", "")
        data = resp.read()
    return data, ctype.lower().startswith("text/html")


def unique_path(directory, filename):
    """Avoid clobbering when two URLs share a basename."""
    if not filename:
        filename = "image"
    candidate = os.path.join(directory, filename)
    if not os.path.exists(candidate):
        return candidate
    root, ext = os.path.splitext(filename)
    i = 1
    while True:
        candidate = os.path.join(directory, f"{root}_{i}{ext}")
        if not os.path.exists(candidate):
            return candidate
        i += 1


class Spider:
    def __init__(self, start_url, recursive, max_depth, path):
        self.recursive = recursive
        self.max_depth = max_depth
        self.path = path
        self.start_host = urlparse(start_url).netloc
        self.visited_pages = set()
        self.downloaded_imgs = set()
        self.count = 0
        os.makedirs(path, exist_ok=True)

    def download_image(self, url):
        url, _ = urldefrag(url)
        if url in self.downloaded_imgs:
            return
        self.downloaded_imgs.add(url)
        try:
            data, _ = fetch(url)
        except (URLError, HTTPError, OSError) as e:
            print(f"  [skip] {url} ({e})", file=sys.stderr)
            return
        name = os.path.basename(urlparse(url).path)
        dest = unique_path(self.path, name)
        try:
            with open(dest, "wb") as f:
                f.write(data)
        except OSError as e:
            print(f"  [skip] cannot write {dest} ({e})", file=sys.stderr)
            return
        self.count += 1
        print(f"  [img ] {url} -> {dest}")

    def crawl(self, url, depth):
        url, _ = urldefrag(url)
        if url in self.visited_pages:
            return
        self.visited_pages.add(url)
        print(f"[page] depth {depth}: {url}")

        try:
            data, is_html = fetch(url)
        except (URLError, HTTPError, OSError) as e:
            print(f"  [skip] {url} ({e})", file=sys.stderr)
            return
        if not is_html:
            return

        parser = LinkImageParser()
        try:
            parser.feed(data.decode("utf-8", errors="ignore"))
        except Exception as e:
            print(f"  [warn] parse error on {url} ({e})", file=sys.stderr)

        # 1) images on this page
        for raw in parser.images:
            absolute = urljoin(url, raw)
            if is_image_url(absolute):
                self.download_image(absolute)

        # 2) anchors: direct image links download; page links recurse
        for raw in parser.links:
            absolute = urljoin(url, raw)
            if is_image_url(absolute):
                self.download_image(absolute)
            elif self.recursive and depth < self.max_depth:
                if urlparse(absolute).netloc == self.start_host:
                    self.crawl(absolute, depth + 1)


def parse_args(argv):
    p = argparse.ArgumentParser(add_help=True, description="Recursively download images from a website.")
    p.add_argument("-r", action="store_true", help="download recursively")
    p.add_argument("-l", type=int, default=5, metavar="N", help="max depth (default 5)")
    p.add_argument("-p", default="./data/", metavar="PATH", help="output directory (default ./data/)")
    p.add_argument("url", help="start URL")
    args = p.parse_args(argv)
    if args.l < 0:
        p.error("-l must be >= 0")
    if not urlparse(args.url).scheme in ("http", "https"):
        p.error("URL must start with http:// or https://")
    return args


def main():
    args = parse_args(sys.argv[1:])
    spider = Spider(args.url, args.r, args.l, args.p)
    spider.crawl(args.url, 0)
    print(f"\nDone. {spider.count} image(s) saved to {os.path.abspath(args.p)}")


if __name__ == "__main__":
    main()