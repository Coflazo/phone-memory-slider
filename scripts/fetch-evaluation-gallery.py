"""Build a licensed, reproducible local gallery from Wikimedia Commons."""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import time
import urllib.parse
import urllib.request
from pathlib import Path

from PIL import Image, ImageEnhance


API = "https://commons.wikimedia.org/w/api.php"
USER_AGENT = "PhoneMemorySlider-Evaluation/1.0 (https://github.com/Coflazo/phone-memory-slider)"


def request_json(params: dict[str, str]) -> dict:
    url = API + "?" + urllib.parse.urlencode(params)
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(request, timeout=45) as response:
        return json.load(response)


def download(url: str, destination: Path) -> str:
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    digest = hashlib.sha256()
    with urllib.request.urlopen(request, timeout=90) as response, destination.open("wb") as output:
        while chunk := response.read(1024 * 1024):
            output.write(chunk)
            digest.update(chunk)
    return digest.hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--count", type=int, default=100)
    parser.add_argument("--output", type=Path, default=Path("evaluation/gallery"))
    args = parser.parse_args()
    if not 100 <= args.count <= 500:
        raise SystemExit("--count must be between 100 and 500")

    originals = args.output / "originals"
    variants = args.output / "variants"
    originals.mkdir(parents=True, exist_ok=True)
    variants.mkdir(parents=True, exist_ok=True)
    manifest: list[dict] = []
    seen: set[str] = set()

    while len(manifest) < args.count:
        payload = request_json(
            {
                "action": "query",
                "format": "json",
                "generator": "random",
                "grnnamespace": "6",
                "grnlimit": "50",
                "prop": "imageinfo",
                "iiprop": "url|mime|size|extmetadata",
                "iiurlwidth": "1600",
            }
        )
        for page in payload.get("query", {}).get("pages", {}).values():
            info = (page.get("imageinfo") or [{}])[0]
            mime = info.get("mime", "")
            url = info.get("thumburl") or info.get("url")
            metadata = info.get("extmetadata", {})
            license_name = metadata.get("LicenseShortName", {}).get("value", "")
            if mime not in {"image/jpeg", "image/png", "image/webp"} or not url or not license_name or url in seen:
                continue
            seen.add(url)
            index = len(manifest) + 1
            suffix = {"image/jpeg": ".jpg", "image/png": ".png", "image/webp": ".webp"}[mime]
            destination = originals / f"original-{index:04d}{suffix}"
            try:
                sha256 = download(url, destination)
                with Image.open(destination) as image:
                    image.verify()
            except Exception:
                destination.unlink(missing_ok=True)
                continue
            manifest.append(
                {
                    "id": index,
                    "file": destination.relative_to(args.output).as_posix(),
                    "source_page": page.get("canonicalurl") or info.get("descriptionurl"),
                    "download_url": url,
                    "author": metadata.get("Artist", {}).get("value", ""),
                    "license": license_name,
                    "license_url": metadata.get("LicenseUrl", {}).get("value", ""),
                    "sha256": sha256,
                }
            )
            if len(manifest) >= args.count:
                break
        time.sleep(0.15)

    variant_manifest: list[dict] = []
    quarter = args.count // 4
    for item in manifest[: quarter * 4]:
        index = item["id"]
        source = args.output / item["file"]
        with Image.open(source) as loaded:
            image = loaded.convert("RGB")
            if index <= quarter:
                destination = variants / f"exact-{index:04d}.jpg"
                shutil.copyfile(source, destination)
                kind = "exact"
            elif index <= quarter * 2:
                destination = variants / f"near-resize-{index:04d}.jpg"
                image.thumbnail((max(64, image.width // 2), max(64, image.height // 2)))
                image.save(destination, "JPEG", quality=88)
                kind = "near"
            elif index <= quarter * 3:
                destination = variants / f"near-tone-{index:04d}.jpg"
                ImageEnhance.Brightness(image).enhance(0.88).save(destination, "JPEG", quality=90)
                kind = "near"
            else:
                destination = variants / f"near-crop-{index:04d}.jpg"
                dx = max(1, image.width // 20)
                dy = max(1, image.height // 20)
                image.crop((dx, dy, image.width - dx, image.height - dy)).save(destination, "JPEG", quality=90)
                kind = "near"
        variant_manifest.append(
            {
                "id": index,
                "file": destination.relative_to(args.output).as_posix(),
                "kind": kind,
                "source_id": index,
                "sha256": hashlib.sha256(destination.read_bytes()).hexdigest(),
            }
        )

    document = {
        "schema": 1,
        "source": "Wikimedia Commons random file API",
        "generated_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "original_count": len(manifest),
        "variant_count": len(variant_manifest),
        "originals": manifest,
        "variants": variant_manifest,
    }
    (args.output / "manifest.json").write_text(json.dumps(document, indent=2), encoding="utf-8")
    print(json.dumps({"originals": len(manifest), "variants": len(variant_manifest)}))


if __name__ == "__main__":
    main()
