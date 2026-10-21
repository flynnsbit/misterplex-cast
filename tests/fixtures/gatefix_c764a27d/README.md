Gatefix corpus of record, per-AU files (T-SPS-GATE-FALLBACK pre-reg v1.4.3 b57a87fb, section 8 T10).

Source: the phase-3 gatefix lab fixtures (wK). Corpus seal MD5SUMS 16a96a18, manifest c764a27d,
README e2050d90; review 00:12 CLEAR, final. The 34 files here are byte-identical copies of the corpus
<fixture>.au0.264 / .au1.264 files (MD5SUMS in this dir; every row is also a row of the corpus seal).

Each fixture is one field changed against its parent (base B0_base 6398deeb: x264 Baseline L3.0 624x480, 2 frames,
ref 1). AU0 = AUD + SPS + PPS + IDR, AU1 = AUD + one P slice. post_gate_valid=no fixtures (G18 x2, G26 x2, N_weighted,
N_interlace) are gate-only: their slice data was copied, not re-encoded, so they are never decoded.

Today's behaviour of record is the gatefix daemon run of record (wR, lab-runs/gatefix-drr-c764a27d-20261006-001512,
RECORD.v2.MD5 aa19e8d0; review 02:46 CLOSED OF RECORD): 17/17 err + AU bytes as in its expect.tsv b62a3ad9.
tests/unit/test_compressed_syntax.cpp asserts that table under the v1 GateCaps.
