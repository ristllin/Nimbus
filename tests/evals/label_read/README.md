# Label-read eval (lane RB-M1)

How accurately each candidate head reads a nutrition label from a real photo, at the two Telegram
renditions (800 and 1,280 px on the long edge), and whether an intake transcription beside the
pixels helps. The numbers are in `tests/evals/measured/labels.json`; they set the photo design of
PRD section 5 (which rendition the head gets, and whether a transcription stays beside it).

## Fixtures

`labels.json` lists 40 real photos of nutrition labels from Open Food Facts, stored in `photos/`
(9.5 MB). Each is the contributor's selected nutrition crop, downscaled to a long edge of at most
1,600 px; the 800 and 1,280 px renditions are made at run time. No owner photo is used.

- License: the photos are CC BY-SA 3.0, by the Open Food Facts contributors. Each row carries its
  product page (`product_url`), the source image (`image_url`) and the attribution line. Reuse them
  under the same license with that attribution.
- Truth: every figure under `columns` was read by eye from the photo, per 100 g or ml (`per100`)
  and per serving (`serving`), exactly as printed ('<0.5' keeps its sign). The OFF structured record
  was only a cross-check; it disagrees with the photo on several products (older recipes), and the
  photo wins. A figure that glare or blur hides is listed under `unscored` and left out. Two
  candidates were dropped (`dropped`): no table in frame, and a line cut at the edge.
- Coverage (`tags`): kJ and kcal on all 40, a serving column on 33, fibre rows on 26, polyols on 8,
  35 labels not in English only (French, German, Dutch, Polish, Czech, Slovak, Romanian, Bulgarian,
  Greek, Italian, Spanish, Portuguese, Finnish, Swedish, Danish), glare, blur, curved packs, angled
  shots, wide strips, rows that do not line up with their names, a %RI column beside the serving
  figures, running text instead of a table, a polyunsaturates row next to no polyols row.

## Arms

| Arm | The head receives |
|---|---|
| P800, P1280 | The photo at that long edge and the engine-2 photo label ('Photo 1 of 1 [p<ref8>] (attached: ...)'). |
| T800, T1280 | The same pixels, and the label carries an intake transcription: '(transcribed by claude-haiku-5-5) <text>', capped at 2,048 bytes as the device caps media text. |
| D800, D1280 | As T, with the transcription exactly as the device makes it today: the caption call stops at 900 output tokens. |

The transcription is the device's fallback caption call (`lib/harness/src/vision_caption.cpp`:
claude-haiku-5-5, effort low, the same prompt) on the 1,280 px rendition, made once per photo and
cached so every head sees the same text. T allows 4,000 output tokens; with 900 (arm D) Haiku stops
at max_tokens on 22 of the 40 labels, mid-table, because its thinking shares the cap.

Every read asks the head to copy the table into a fixed JSON shape (structured output on both
wires; Anthropic heads at effort medium, OpenAI heads with image detail high), one column per basis.

## Scoring (`score.py`)

A field is one printed figure of one column. It is right when the read figure equals the printed
one after unit and decimal-comma normalisation; a missing or added '<', a figure from the wrong
column, a missing column or a failed read are wrong. Also counted: kJ/kcal confusions (the kJ
figure given as kcal, or the pair swapped), protein misreads, and extra figures (a value for a row
the label does not print, sodium aside). Rates carry an exact one-sided 95% bound
(`tests/evals/behaviour/stats.py`).

## Decisions (`label_report.py`)

- Rendition: 800 px, unless 1,280 px reads at least one percentage point more fields right on
  claude-sonnet-5-5 or claude-haiku-5-5, pooled over the P and T arms.
- Transcription (PRD section 5): kept unless the head alone is at least as accurate as pixels plus
  transcription and at or above 95% field accuracy, at the chosen rendition. One intake path serves
  every head, so it is kept if the rule keeps it on either Claude head.

## Run it

```bash
# free self-test: no keys, no network (Pillow for the rendition test)
python3 tests/evals/label_read/test_label_read_offline.py

# PAID: both transcription caches once, then one head (reruns skip stored reads)
python tests/evals/label_read/read_labels.py --head claude-haiku-5-5 --evidence <dir>/claude-haiku-5-5 \
  --transcripts <dir>/transcripts.jsonl --device-transcripts <dir>/transcripts-device900.jsonl \
  --env-file <dotenv> --max-usd 1 --transcribe-only
python tests/evals/label_read/read_labels.py --head claude-sonnet-5-5 --arms P800,P1280,T800,T1280,D800,D1280 \
  --evidence <dir>/claude-sonnet-5-5 --transcripts <dir>/transcripts.jsonl \
  --device-transcripts <dir>/transcripts-device900.jsonl --env-file <dotenv> --max-usd 4
python tests/evals/label_read/label_report.py --evidence <dir> --out tests/evals/measured/labels.json --md <dir>/RESULTS.md
```

The live run is never part of CI. Keys are read from the env file inside the process and never
printed; error text is redacted before it is stored.
