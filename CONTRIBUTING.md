# Contributing to VIcky-Blue

VIcky-Blue is a fork of [KytyPS5](https://github.com/KytyPS5/KytyPS5), licensed under
GPL-2.0. These rules apply to everything that ends up in or around this repository: code,
comments, commit messages, branch names, pull requests, issues, discussions, review comments,
screenshots and logs.

## 1. Do not name the target title

This fork concentrates on a single title. That title, its series, and any abbreviation,
numeral variant, nickname or obvious misspelling of either must not appear anywhere in this
repository or its tracker.

- Write "the target title" instead.
- Scrub logs, paths and screenshots before posting them. Dump folder names and window titles
  are the usual leaks.
- Do not work around the automated check with creative spelling. If a reader can tell which
  game you mean, it is a violation.

Commits and pull requests that break this rule are rejected and must be rewritten, not just
followed up with a fix. Issues and comments that break it are edited or removed.

## 2. No game files

Never commit, attach or link to:

- game dumps or any part of one (executables, modules, archives, `sce_sys` / `sce_module`
  contents, assets, shaders extracted from a game, save data);
- console firmware, system modules, keys or decrypted system software;
- memory dumps, captures or traces that embed copyrighted game data.

Short disassembly or log excerpts needed to explain a fix are fine as long as they follow
rule 1. Test fixtures must be synthetic.

## 3. AI-assisted contributions are allowed

You may use AI tools to write code, tests, documentation and commit messages. You remain
responsible for what you submit: you must understand it, have built and tested it, and it
must follow every other rule here. AI co-author trailers are fine.

## Joining in

1. **Pick an issue.** The [issue list](https://github.com/wololo-wololo/VIcky-Blue/issues) is the list of work.
   [`blocker`](https://github.com/wololo-wololo/VIcky-Blue/labels/blocker) issues stop the target title right now,
   [`workaround-debt`](https://github.com/wololo-wololo/VIcky-Blue/labels/workaround-debt) issues replace a temporary hack with a real
   implementation, and [`good first issue`](https://github.com/wololo-wololo/VIcky-Blue/labels/good%20first%20issue) is the easy way
   in. Milestones show which goal an issue serves.
2. **Claim it.** Comment `/claim` on the issue. You become its assignee and the overview shows
   it as taken, so two people do not do the same work. If it is already held, the bot says by
   whom. Comment `/unclaim` when you stop; a claim with no activity for two weeks may be
   released by a maintainer. A claim only counts on GitHub, not in chat.
3. **Use the issue's branch.** Every issue has a linked branch named
   `<area>/<issue number>-<short-name>`, shown in the issue's Development sidebar.
   Collaborators push to it directly. Everyone else forks the repository, branches from
   `main` with the same name, and opens a pull request from the fork.
4. **Commit small, push often.** Use `area: summary` subjects and say what changed and why.
   Open a draft pull request early so others can see the direction.
5. **Open the pull request against `main`** with `Closes #<issue>` in the description. Show
   what changed for the target title, with a scrubbed log excerpt.
6. **Update the README status** in the same pull request when your change moves the project
   to a new stage or removes a workaround.

Found something new? Open an issue with the *Blocker or bug* or *Task* template before
starting, so it shows up in the list.

You need your own legally obtained copy of the target title to test. Nobody here can give you
game files or tell you where to get them.

## Reporting what you find

Every test run should feed back into the issue list, so that the list always shows what is
actually broken.

1. Run the target title and keep the emulator's output in a log file.
2. Run the triage script on it:

   ```sh
   python scripts/triage-log.py boot.log
   ```

   It prints the problems it recognises: the fatal error, unimplemented library functions,
   skipped shaders, sampler fallbacks and event waits that never complete. Paths are removed
   and the log itself never leaves your machine.
3. Collaborators can file the result directly:

   ```sh
   python scripts/triage-log.py boot.log --file-issues
   ```

   A problem nobody has reported yet becomes a new issue labelled `needs-triage`. One that
   already has an open issue gets a "still present" comment, and one whose issue was closed is
   reopened as a regression. Everyone else opens a *Blocker or bug* issue and pastes the
   report.
4. A maintainer reviews each `needs-triage` issue: confirms it, adds where to look, sets a
   milestone and labels, and removes `needs-triage`.

The pinned "Start here" issue is rebuilt automatically from the open issues whenever one
changes, with new and unreviewed problems at the top. To be notified about new problems, use
**Watch → Custom → Issues** on the repository.

Something the script does not recognise, such as wrong rendering or a hang with no error,
still needs an issue written by hand.

## Enforcement

`scripts/policy-check.sh` implements rules 1 and 2. It runs:

- locally through [pre-commit](https://pre-commit.com) — install both hook types once:

  ```sh
  pre-commit install --hook-type pre-commit --hook-type commit-msg
  ```

- in CI (`.github/workflows/policy.yml`) on every push, pull request, issue and comment.

The automated check is a floor, not the definition of the rule. Maintainers have the final
say.

## Code

Follow the existing style (`.clang-format`, `.clang-tidy`) and keep commits focused. Commit
subjects use the upstream `area: summary` form.
