# AGENTS.md

## Project language

- English is the authoritative language for all repository content.
- Write documentation, source code, comments, identifiers, configuration keys,
  test names, commit messages, and user-facing program output in English.
- Preserve standardized GNSS terminology, protocol field names, receiver
  message names, filenames, and externally defined identifiers exactly as
  specified by their source standards.
- Existing non-English text should be translated when the surrounding file is
  modified, unless it is quoted source material or test data whose exact bytes
  are significant.

## Data handling

- Treat GNSS archives under `/hdd` as read-only preservation masters.
- Keep generated and large observation products out of Git.
- Every derived artifact must be reproducible from source data, pinned tools,
  versioned configuration, and recorded provenance.
- Never infer observation coverage or time scale solely from filenames; inspect
  payload timestamps and preserve both GNSS time and UTC where applicable.

## Toolchain

- References to RTKLIB mean the RTKLIB-EX `main` branch from
  `rtklibexplorer/RTKLIB`, not upstream `tomojitakasu/RTKLIB`.
- Pin exact tool revisions or container digests for production processing.
- Preserve original observation codes and receiver message provenance during
  normalization.
