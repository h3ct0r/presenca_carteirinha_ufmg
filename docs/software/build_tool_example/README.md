# Sample config-builder inputs

Real-shaped input files for trying out [`tools/config-builder/`](../../../tools/config-builder/)
without needing access to Moodle or the UFMG systems.

| File | Feed it to |
|---|---|
| `20262_DCC219_TE1.csv`, `20262_DCC219_TN2.csv` | **Import Diário de Classe** — two turmas of the same course and semester, so they merge into one class with each roster entry tagged by turma. |
| `20262_DCC230_TU.csv` | **Import Diário de Classe** — a second, separate class. |
| `images.tar` | **Student photos (Moodle)** — a photo archive in the shape [MOODLE_PHOTOS.md](../MOODLE_PHOTOS.md) produces. |
| `img_alunos/` | The same photos unpacked, for inspection. |

The CSVs are semicolon-separated with a `PERIODO;ATIVIDADE;TURMA` header block,
which is the format the importer expects.
