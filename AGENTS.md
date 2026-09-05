# AGENTS.md

## Project language

- Write project files in English, including documentation, source code,
  comments, identifiers, configuration keys, and test names.
- Preserve standardized GNSS terminology, protocol field names, receiver
  message names, filenames, and externally defined identifiers exactly as
  specified by their source standards.
- Existing non-English text should be translated when the surrounding file is
  modified, unless it is quoted source material or test data whose exact bytes
  are significant.
- This language policy applies only to files in the repository. It does not
  constrain communication with users; use the language appropriate for the
  current conversation.

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
