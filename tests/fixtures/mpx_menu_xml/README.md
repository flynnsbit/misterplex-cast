# mpx-menu library XML fixtures (test_library_parse_corpus.py)

- `l1/`: 24 mpx-menu P0b L1 pages (release mpx-menu cd431cb3 L1 run, 2026-10-04 14:30).
  232 Directory/Video rows with the shipping labels (NEXT / PREV / SERVERS).
- `p0/`: 11 mpx-menu P0 pages with the original labels `SERVERS >`, `NEXT >`, `&lt; PREV`.
  dbcc42fa/2164c4ad `appendTags` cut those tags at the first `>` and `xmlAttr` did not decode entities:
  3 rows were dropped and one title drew `&lt; PREV`.
- The two MpxPlay ticket pages are not included. They carry token fields; the saved copies had them redacted
  to a literal `<tok>`, which is not well-formed XML.
- Leak scan before commit: no token attributes, IPs, URLs, plex.direct hosts or 40-hex IDs. The pages hold
  only titles and opaque menu keys.
