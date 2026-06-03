---
name: mapreduce-session-end
description: End-of-session workflow for the local mapreduce C project. Use when the programmer finishes a programming session in /Users/enricomelis/code/mapreduce and needs a concise progress report, a proposed docs/diario.md update for approval before writing, documentation of architectural and technical choices with alternatives, and a recall check to verify understanding of the current project status.
---

# MapReduce Session End

## Overview

Use this skill to close a programming session cleanly: summarize what changed, connect it to `docs/Testo.md`, draft a diary update, and verify that the programmer can explain the state and design decisions.

## Project Scope

- Work only in `/Users/enricomelis/code/mapreduce`.
- Treat `docs/Testo.md` as the source of truth for the base project.
- Update `docs/diario.md` only after showing a draft and receiving explicit approval.
- Respect `AGENTS.md`: guide learning first, use `gcc`, avoid running `make`, and avoid generating code unless the user explicitly asks for a small didactic change.

## Closing Workflow

1. Collect the final state:
   - read `AGENTS.md`;
   - read the relevant parts of `docs/Testo.md`;
   - read `docs/diario.md`;
   - check `git status --short`;
   - inspect relevant diffs and touched files.

2. Produce a progress report:
   - describe what advanced during the session;
   - distinguish implemented behavior, tests, documentation, and open work;
   - mention checks run with `gcc` or other direct commands;
   - if `make` would be useful, ask the programmer to run it in the dev container instead of running it locally.

3. Document decisions and alternatives:
   - list each meaningful architectural or technical choice made;
   - explain why that choice was selected;
   - name at least one plausible alternative when it exists;
   - state the tradeoff or consequence for later development.

4. Draft the diary update:
   - prepare text for `docs/diario.md` under the current date;
   - keep the draft concrete and compatible with the existing diary style;
   - include sections only when useful: `Stato attuale`, `Scelte tecniche`, `Verifiche`, `Prossimi passi`, `Punti da saper spiegare`, `Note aperte`;
   - show the draft to the programmer and ask for approval before editing the file.

5. Apply the diary update only after approval:
   - use a minimal edit;
   - preserve previous entries;
   - avoid rewriting unrelated diary content;
   - after editing, show what changed and re-check `git status --short`.

6. Run a recall check:
   - ask 2 to 4 focused questions about the final state, the main choices, and the next risk;
   - let the programmer answer before giving full corrections;
   - correct misunderstandings briefly and explicitly.

## Diary Draft Shape

Use this structure when it fits the session:

- `Avanzamento`: concrete behavior, files, and tests affected.
- `Scelte tecniche`: choices, alternatives, and reasons.
- `Verifiche`: commands run and what they proved.
- `Prossimi passi`: small next steps tied to `docs/Testo.md`.
- `Punti da saper spiegare`: recall prompts the programmer should be able to answer.
- `Note aperte`: unresolved decisions or risks.

## Output Shape

Start with a concise report, then show the proposed diary entry in a fenced Markdown block, then ask for approval to write it. Do not edit `docs/diario.md` in the same response that first shows the draft.
