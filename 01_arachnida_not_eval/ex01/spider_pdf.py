#!/usr/bin/env python3
"""
spider (PDF edition) - recursively download PDF files from a website.

Usage:
    ./spider_pdf.py [-r] [-l N] [-p PATH] URL

Options:
    -r          follow links recursively to discover more PDFs
    -l N        maximum recursion depth (default 5, only meaningful with -r)
    -p PATH     directory where PDFs are saved (default ./data/)

This is a variant of the image spider: instead of images it collects PDF
documents. PDFs are normally referenced from <a href>, but also from
<embed>, <object> and <iframe>, so those are checked too. Recursion still
follows same-host HTML pages. Only the Python standard library is used.
"""

import argparse
import os
import sys
from html.parser import HTMLParser
from urllib.parse import urljoin, urlparse, urldefrag, unquote
from urllib.request import Request, urlopen
from urllib.error import URLError, HTTPError

TARGET_EXTENSIONS = (".pdf",)          # what we download (easy to extend)
USER_AGENT = "arachnida-spider-pdf/1.0"
TIMEOUT = 20

# links with these extensions are not HTML pages -> never recurse into them
ASSET_EXTENSIONS = (
    ".pdf", ".js", ".css", ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".svg",
    ".webp", ".ico", ".mp4", ".webm", ".mov", ".avi", ".mp3", ".wav", ".zip",
    ".gz", ".woff", ".woff2", ".ttf", ".eot", ".json", ".xml",
    ".txt", ".csv", ".doc", ".docx", ".xls", ".xlsx", ".ppt", ".pptx",
    ".rtf", ".odt", ".rar", ".7z", ".tar",
)


class PdfLinkParser(HTMLParser):
    """Collect candidate document URLs and anchor links from a page.

    We write the tag handling ourselves so the logic is our own:
      - <a href>                       -> links  (recurse + download if PDF)
      - <link>/<embed>/<object>/<iframe> -> docs (download only, if PDF)
    """

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.docs = []    # URLs that might be a document (download only)
        self.links = []   # <a href> URLs (recursion + direct PDF links)

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if tag == "a" and attrs.get("href"):
            self.links.append(attrs["href"])
        elif tag == "link" and attrs.get("href"):
            self.docs.append(attrs["href"])
        elif tag in ("embed", "iframe") and attrs.get("src"):
            self.docs.append(attrs["src"])
        elif tag == "object" and attrs.get("data"):
            self.docs.append(attrs["data"])


def path_of(url):
    return urlparse(url).path.lower()


def is_target_url(url):
    return path_of(url).endswith(TARGET_EXTENSIONS)


def is_asset_url(url):
    return path_of(url).endswith(ASSET_EXTENSIONS)


def canonical(url):
    """Give a host-only URL the "/" path so host and host/ don't diverge."""
    url, _ = urldefrag(url)
    parts = urlparse(url)
    if parts.scheme in ("http", "https") and parts.path == "":
        url += "/"
    return url


def fetch(url):
    """Return (content_bytes, content_type). Raises on network error."""
    req = Request(url, headers={"User-Agent": USER_AGENT})
    with urlopen(req, timeout=TIMEOUT) as resp:
        ctype = resp.headers.get("Content-Type", "").lower()
        data = resp.read()
    return data, ctype


def unique_path(directory, filename):
    if not filename:
        filename = "document.pdf"
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
        self.downloaded = set()
        self.count = 0
        os.makedirs(path, exist_ok=True)

    def download(self, url):
        url = canonical(url)
        if url in self.downloaded:
            return
        self.downloaded.add(url)
        try:
            data, ctype = fetch(url)
        except (URLError, HTTPError, OSError) as e:
            print(f"  [skip] {url} ({e})", file=sys.stderr)
            return
        # trust the extension, but also accept a PDF content-type as a sanity note
        name = unquote(os.path.basename(urlparse(url).path)) or "document.pdf"
        dest = unique_path(self.path, name)
        try:
            with open(dest, "wb") as f:
                f.write(data)
        except OSError as e:
            print(f"  [skip] cannot write {dest} ({e})", file=sys.stderr)
            return
        self.count += 1
        note = "" if "pdf" in ctype else "  (warning: not served as application/pdf)"
        print(f"  [pdf ] {url} -> {dest}{note}")

    def crawl(self, url, depth):
        url = canonical(url)
        if url in self.visited_pages:
            return
        self.visited_pages.add(url)
        print(f"[page] depth {depth}: {url}")

        try:
            data, ctype = fetch(url)
        except (URLError, HTTPError, OSError) as e:
            print(f"  [skip] {url} ({e})", file=sys.stderr)
            return
        if not ctype.startswith("text/html"):
            return

        parser = PdfLinkParser()
        try:
            parser.feed(data.decode("utf-8", errors="ignore"))
        except Exception as e:
            print(f"  [warn] parse error on {url} ({e})", file=sys.stderr)

        # 1) document references (never recursed into)
        for raw in parser.docs:
            absolute = urljoin(url, raw)
            if is_target_url(absolute):
                self.download(absolute)

        # 2) anchors: PDF links download; HTML links recurse
        for raw in parser.links:
            absolute = canonical(urljoin(url, raw))
            if is_target_url(absolute):
                self.download(absolute)
            elif (self.recursive and depth < self.max_depth
                  and not is_asset_url(absolute)
                  and urlparse(absolute).netloc == self.start_host):
                self.crawl(absolute, depth + 1)


def parse_args(argv):
    p = argparse.ArgumentParser(description="Recursively download PDFs from a website.")
    p.add_argument("-r", action="store_true", help="download recursively")
    p.add_argument("-l", type=int, default=5, metavar="N", help="max depth (default 5)")
    p.add_argument("-p", default="./data/", metavar="PATH", help="output directory (default ./data/)")
    p.add_argument("url", help="start URL")
    args = p.parse_args(argv)
    if args.l < 0:
        p.error("-l must be >= 0")
    if urlparse(args.url).scheme not in ("http", "https"):
        p.error("URL must start with http:// or https://")
    return args


def main():
    args = parse_args(sys.argv[1:])
    start = canonical(args.url)
    spider = Spider(start, args.r, args.l, args.p)
    if is_target_url(start):          # the URL points straight at a PDF
        spider.download(start)
    else:                             # it's a page: crawl it for PDFs
        spider.crawl(start, 0)
    print(f"\nDone. {spider.count} PDF(s) saved to {os.path.abspath(args.p)}")


if __name__ == "__main__":
    main()