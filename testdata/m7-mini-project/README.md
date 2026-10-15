<!-- SPDX-License-Identifier: GPL-3.0-only -->
# Lightweight multi-document fixture

Three original, four-measure BMS charts at 120 BPM, with 8 / 12 / 8 notes:

- `01-easy.bms`: Base36, sparse notes.
- `02-hard.bms`: Base36, a few extra notes; deliberately different WAV-slot mappings.
- `03-base62.bms`: Base62, distinguishes `0A` (10) from `0a` (36).

No third-party chart/audio was copied. Shared resource names are basenames, not
paths into any developer's local directory. This directory contains source text
only: automated tests copy it to scratch storage and generate three tiny WAVs
there. Do not save test edits into these canonical files.

The closed-loop tests exercise open/reuse, fixed document binding, inactive-document
edits, independent undo/dirty state, save/reopen and offline rendering. Real
complex-chart tests remain the full regression; this fixture does not replace them.
