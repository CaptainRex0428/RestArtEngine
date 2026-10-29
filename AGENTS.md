# RestArtEngine workspace entry

This public file is a lightweight entry point. Engine architecture and build instructions are in [README.md](README.md) and [docs/Architecture.md](docs/Architecture.md).

## Optional collaboration workspace

Continue working from the **RAEngine root**. If `local/docs/AgentEntry.md` exists, read it before project collaboration work, then read the relevant development status, task rules, and skills it references. The `local/` directory is an independent collaboration Git repository and is optional for building and using the engine.

Project skills remain at `.agents/skills/`; project agent definitions remain at `.codex/agents/`. These are ignored runtime copies installed from `local/workspace/`. Keep the existing names and invocation paths. Do not execute skills from the storage mirror or relocate the user's working directory to `local/`.

From the engine root, `./scripts/Sync-Workspace.ps1 -Mode Status` checks the copies. `-Mode Install` installs workspace versions; `-Mode Collect` collects local edits. Read `local/docs/WorkspaceSync.md` before synchronizing. If the collaboration workspace is absent, report that limitation for tasks requiring it; ordinary public engine work does not require workspace files.

## Publication boundary

The separate RestArtEngine_Workspace repository is Public by owner choice. This does not move its full collaboration content into the engine repository or include machine settings and credentials.

This root entry and the generic synchronization wrapper may be committed to the public engine repository. Full collaboration instructions, skill bodies, agent definitions, progress records, development logs and machine settings must stay out of that repository. Do not force-add `local/`, `.agents/` or `.codex/`. Credentials and machine configuration are not synchronization inputs.

Keep public technical documentation usable without the collaboration workspace. Preserve existing changes from users and other tasks; inspect the appropriate repository's status before modifying or committing files.
