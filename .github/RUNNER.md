# Self-hosted CI runners

CI (`.github/workflows/ci.yml`) runs on our own server, not on GitHub's
machines, because it needs Catapult, VCS and their licenses. A *runner* is
GitHub's open-source agent (https://github.com/actions/runner) that does this:
it keeps an outgoing HTTPS connection to GitHub open, asks for queued jobs,
runs them locally and streams the logs and results back. The server needs no
open port or public address, only outgoing access to github.com.

One runner runs one job at a time, so several copies are installed side by
side to run jobs in parallel. Every job uses `--num_processes 16`, so four
copies use about 64 cores.

## Current installation

| What | Where |
| --- | --- |
| Server | `r8cad-tsmc7.stanford.edu` (Rocky Linux 8), user `jeffreyy` |
| Runner copies | `~/actions-runner/1` … `~/actions-runner/4`, named `tsmc7-1` … `tsmc7-4`, labels `self-hosted,Linux,X64,tsmc7` |
| Runner version | 2.338.1 (updates itself) |
| Per-run conda envs | `~/actions-runner/envs/ci-env-<run id>` (created by the `setup` job, removed by `cleanup`) |
| Job checkouts and builds | `~/actions-runner/<n>/_work/` |
| Start script | `~/actions-runner/start.sh` |
| Logs | `~/actions-runner/<n>/runner.log`, detailed logs in `~/actions-runner/<n>/_diag/` |

No root access is needed for any of it.

## Setting it up on a new server

Requirements: outgoing HTTPS to github.com, `~/miniforge3` (the jobs create
their conda envs with it), `modulecmd` with the CAD modules `ci.yml` loads,
SSH access to `code.stanford.edu` (`physical-design` and `internal-tech`
submodules), and a few GB per copy plus about 15 GB per running RTL job.

1. Download the runner and check it against the SHA-256 on the release page
   (https://github.com/actions/runner/releases):

   ```bash
   mkdir ~/actions-runner && cd ~/actions-runner
   T=actions-runner-linux-x64-2.338.1.tar.gz
   curl -sSfL -o $T https://github.com/actions/runner/releases/download/v2.338.1/$T
   echo "08e9ccb3604d2462c24b5e33604dc30810296ca15cc65f23cffd4317eb144c37  $T" | sha256sum -c
   for i in 1 2 3 4; do mkdir $i && tar xzf $T -C $i; done
   ```

2. Register each copy with the repository. Get a registration token from
   https://github.com/jeffreyyu0602/voyager-lite/settings/actions/runners/new
   (the string after `--token`; it expires after one hour and works for all
   copies). Run from a login shell: the runner saves the current `PATH` into
   `<n>/.path` and every job uses it, so it must include conda and
   `modulecmd`.

   ```bash
   for i in 1 2 3 4; do
       cd ~/actions-runner/$i
       bash -lc "./config.sh --unattended --url https://github.com/jeffreyyu0602/voyager-lite --token <TOKEN> --name <server>-$i --labels <server>"
   done
   ```

3. Give the jobs a shared folder for their conda envs. `ci.yml` and
   `ci/env.sh` read `CI_ENV_ROOT`; each copy's `.env` file sets it for its
   jobs:

   ```bash
   mkdir ~/actions-runner/envs
   for i in 1 2 3 4; do echo "CI_ENV_ROOT=$HOME/actions-runner/envs" >> ~/actions-runner/$i/.env; done
   ```

4. Keep the copies running. Without root there is no system service, so
   cron starts them: `start.sh` starts every registered copy that is not
   already running, at boot and every 10 minutes (which also restarts a
   crashed one). Processes started this way belong to no login session, so
   logging out does not stop them.

   `~/actions-runner/start.sh`:

   ```bash
   #!/bin/bash
   # Start every registered runner copy in this folder that is not running.
   cd "$(dirname "$0")"
   for dir in */; do
       dir=${dir%/}
       [ -f "$dir/.runner" ] || continue
       pgrep -u "$USER" -f "$PWD/$dir/bin/Runner.Listener" > /dev/null && continue
       setsid nohup "$PWD/$dir/run.sh" > "$PWD/$dir/runner.log" 2>&1 < /dev/null &
   done
   ```

   ```bash
   chmod +x ~/actions-runner/start.sh
   # Replaces the whole crontab: if `crontab -l` shows other lines, add these
   # two with `crontab -e` instead.
   printf '@reboot $HOME/actions-runner/start.sh\n*/10 * * * * $HOME/actions-runner/start.sh\n' | crontab -
   ~/actions-runner/start.sh
   ```

5. Check: `tail -1 ~/actions-runner/*/runner.log` prints `Listening for
   Jobs` for every copy, and the runners page on GitHub shows them as Idle.

## GitHub settings (repository owner, web UI)

- **Settings → Actions → General → Fork pull request workflows**: require
  approval for all outside contributors. A pull request from a fork runs its
  own copy of `ci.yml`, so a check inside the workflow cannot stop it; without
  this setting a stranger's pull request runs on the server with the
  account's SSH keys and CAD tools.
- **Settings → Rules → `main`**: "Require status checks to pass", listing
  `check_results` and the simulation jobs, so a slowdown found by
  `check_pull_request.py` blocks the merge until someone reviews it (an admin
  can then bypass).

## Running a CI test row by hand

Each simulation row in `ci.yml` is one `ci/run.sh` call, so a failing row
can be rerun in a normal checkout with the CAD tools and a conda env loaded,
plus the job's `env:` settings:

```bash
CONV2D_IM2COL=1 CLOCK_PERIOD=5 TECHNOLOGY=generic ci/run.sh MXNF4-64x64 rtl llama_prefill_mp
```

## Maintenance

- **Stop all copies**: remove the two lines with `crontab -e` first (or cron
  restarts them within 10 minutes), then
  `pkill -u $USER -f '[a]ctions-runner/./bin/Runner.Listener'`.
- **Add a copy**: unpack the tarball into `~/actions-runner/5`, register it
  (step 2, new token), add the `.env` line (step 3), run `start.sh`.
- **Remove a copy**: stop it, then in its folder run
  `./config.sh remove --token <TOKEN>` with a token from the runners page
  ("Remove" on that runner), and delete the folder.
- **Move to another server**: remove the copies here, then follow the setup
  steps there.
- **Leftover conda envs**: `cleanup` deletes each run's env even when the run
  fails or is cancelled; anything still in `~/actions-runner/envs` with no
  run in progress can be deleted.
