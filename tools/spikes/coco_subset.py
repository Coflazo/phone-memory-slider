"""Pick a stratified COCO val2017 subset labeled by dominant human-annotated supercategory, then download it.

Labels come from COCO's human instance annotations (largest total mask area per image), never from
the encoder, so taste evaluations built on them are not circular. Dev-time network only.
Run: .tools/venv/Scripts/python.exe tools/spikes/coco_subset.py [per_group]
"""
import collections, concurrent.futures, json, pathlib, random, sys, urllib.request

ROOT = pathlib.Path(__file__).resolve().parents[2]
COCO = ROOT / ".tools" / "datasets" / "coco"
URL = "https://huggingface.co/datasets/merve/coco/resolve/main/val2017/{}"


def main(per_group=170):
    ann = json.loads((COCO / "instances_val2017.json").read_text())
    super_of = {c["id"]: c["supercategory"] for c in ann["categories"]}
    name_of = {c["id"]: c["name"] for c in ann["categories"]}
    area = collections.defaultdict(lambda: collections.Counter())
    for a in ann["annotations"]:
        area[a["image_id"]][a["category_id"]] += a["area"]
    rows = []
    for img in ann["images"]:
        counts = area.get(img["id"])
        if not counts:
            continue
        by_super = collections.Counter()
        for cid, ar in counts.items():
            by_super[super_of[cid]] += ar
        sup, sup_area = by_super.most_common(1)[0]
        if sup_area / (img["width"] * img["height"]) < 0.25:
            continue  # no clearly dominant subject; ambiguous label
        rows.append({
            "file": img["file_name"], "id": img["id"], "license": img["license"],
            "width": img["width"], "height": img["height"], "supercategory": sup,
            "category": name_of[counts.most_common(1)[0][0]],
            "categories": sorted({name_of[c] for c in counts}),
        })
    rng = random.Random(7)
    groups = collections.defaultdict(list)
    for r in rows:
        groups[r["supercategory"]].append(r)
    chosen = []
    for sup, items in sorted(groups.items()):
        rng.shuffle(items)
        chosen += items[:per_group]
        print(f"{sup:12s} available={len(items):5d} chosen={min(per_group, len(items))}")
    out = COCO / "val2017"
    out.mkdir(exist_ok=True)

    def fetch(r):
        dst = out / r["file"]
        if not dst.exists():
            urllib.request.urlretrieve(URL.format(r["file"]), dst)
        return r["file"]

    with concurrent.futures.ThreadPoolExecutor(8) as pool:
        for i, _ in enumerate(pool.map(fetch, chosen), 1):
            if i % 250 == 0:
                print(f"downloaded {i}/{len(chosen)}", flush=True)
    (COCO / "subset.json").write_text(json.dumps(chosen, indent=1))
    print(f"wrote {len(chosen)} rows to {COCO / 'subset.json'}")


if __name__ == "__main__":
    main(int(sys.argv[1]) if len(sys.argv) > 1 else 170)
