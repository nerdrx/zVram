#!/usr/bin/env node
// Exercise NX Hub's real prefix engine without touching the user's install.
"use strict";
const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const { execFileSync } = require("node:child_process");

async function main() {
  const [hubRoot, firstArchive, secondArchive] = process.argv.slice(2).map(p => path.resolve(p));
  assert(hubRoot && firstArchive && secondArchive, "usage: check_hub.cjs HUB_ROOT FIRST_TARBALL UPDATE_TARBALL");
  const updateVersion = path.basename(secondArchive).match(/^zvram-(.+)-linux-x86_64\.tar\.gz$/)?.[1];
  assert(updateVersion && updateVersion !== "0.3.0", "update tarball must contain a newer fixture version");
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "zvram-hub-check-"));
  const home = path.join(root, "home");
  for (const dir of [home, path.join(home, ".local/bin")]) fs.mkdirSync(dir, { recursive: true });
  Object.assign(process.env, {
    HOME: home, XDG_DATA_HOME: path.join(home, ".local/share"),
    XDG_STATE_HOME: path.join(home, ".local/state"), XDG_CONFIG_HOME: path.join(home, ".config"),
    ZVRAM_MANAGER_HOME: path.join(home, ".local/state/zvram"), PYTHONDONTWRITEBYTECODE: "1",
    NX_HUB_DATA_DIR: path.join(root, "hub-data"), NX_HUB_INSTALL_ROOT: path.join(root, "Applications")
  });
  const engine = require(path.join(hubRoot, "src/main/install/engine"));
  const validator = require(path.join(hubRoot, "src/main/manifest"));
  const manifest = JSON.parse(fs.readFileSync(path.join(__dirname, "../nx-app.json"), "utf8"));
  const checked = validator.validate(manifest, { trusted: true });
  assert(checked.ok && checked.manifest, JSON.stringify(checked));
  const spec = checked.manifest.artifacts[0];
  assert.equal(spec.kind, "tarball-prefix");
  const prefix = path.join(home, ".local");
  const launcher = path.join(prefix, "bin/zvram");
  const development = path.join(root, "development-zvram");
  fs.writeFileSync(development, "development checkout sentinel\n");
  fs.symlinkSync(development, launcher);
  const foreign = path.join(prefix, "bin/foreign-tool");
  fs.writeFileSync(foreign, "unrelated tool\n");
  const app = { id: "zvram", name: "zVram", repo: "nerdrx/zVram" };
  const artifact = { ...spec, id: "tarball-prefix-linux", prefix, launchCmd: launcher + " gui", version: "0.3.0" };
  const logs = [];
  const ctx = { installRoot: process.env.NX_HUB_INSTALL_ROOT, dataDir: process.env.NX_HUB_DATA_DIR,
    settings: {}, log: line => logs.push(line), emitProgress() {} };
  const python = (script, args = []) => execFileSync("python3", ["-c", script, ...args], { encoding: "utf8", timeout: 15000 }).trim();
  const managerScript = () => path.join(path.dirname(fs.realpathSync(launcher)), "zvram_manager.py");
  const manager = (script, code) => python("import importlib.util,sys,json,time\ns=importlib.util.spec_from_file_location('zvram_manager',sys.argv[1]);m=importlib.util.module_from_spec(s);s.loader.exec_module(m)\nx=m.Manager()\n" + code, [script]);
  const sourceManager = path.resolve(__dirname, "../zvram_manager.py");
  let stopped = false;
  try {
    const installed = await engine.install({ app, artifact, filePath: firstArchive, ctx });
    assert(installed.launchable);
    assert.equal(fs.readFileSync(development, "utf8"), "development checkout sentinel\n");
    const firstTarget = fs.realpathSync(launcher);
    const firstVersion = execFileSync(launcher, ["--version"], { encoding: "utf8" }).trim();
    assert(firstVersion.includes("0.3.0"), firstVersion);
    const oldManager = managerScript();
    manager(oldManager, "x.save_profile({'name':'hub-sleeper','mode':'native','min_available_mib':1,'command':[sys.executable,'-c','import time;time.sleep(120)']})\nx.start('hub-sleeper')\nfor _ in range(50):\n j=m.read_json(x.job_path('hub-sleeper'),{})\n if j.get('child_pid'): break\n time.sleep(.1)\nassert m.owned_worker(j) and j.get('child_pid'),j\nprint(json.dumps(j))");
    const profiles = path.join(process.env.ZVRAM_MANAGER_HOME, "profiles.json");
    const profileBytes = fs.readFileSync(profiles);
    const stateSentinel = path.join(process.env.ZVRAM_MANAGER_HOME, "user-note");
    fs.writeFileSync(stateSentinel, "preserve user state\n");
    artifact.version = updateVersion;
    const updated = await engine.install({ app, artifact, filePath: secondArchive, ctx });
    assert(updated.launchable);
    assert.notEqual(fs.realpathSync(launcher), firstTarget, "update must switch versioned payload");
    const newVersion = execFileSync(launcher, ["--version"], { encoding: "utf8" }).trim();
    assert(newVersion.includes(updateVersion), newVersion);
    manager(managerScript(), "j=m.read_json(x.job_path('hub-sleeper'),{})\nassert m.owned_worker(j),j\nassert b'import time;time.sleep(120)' in m.Path('/proc',str(j['child_pid']),'cmdline').read_bytes()\nassert x.list_profiles()[0]['running']\nassert x.stop('hub-sleeper')\nfor _ in range(80):\n j=m.read_json(x.job_path('hub-sleeper'),{})\n if not m.owned_worker(j): break\n time.sleep(.1)\nassert not m.owned_worker(j),j\nprint('stopped')");
    stopped = true;
    assert.deepEqual(fs.readFileSync(profiles), profileBytes);
    await engine.uninstall({ app, artifact, installedPath: updated.path, ctx });
    assert(!fs.existsSync(launcher));
    assert.equal(fs.readFileSync(development, "utf8"), "development checkout sentinel\n");
    assert.equal(fs.readFileSync(foreign, "utf8"), "unrelated tool\n");
    assert.deepEqual(fs.readFileSync(profiles), profileBytes);
    assert.equal(fs.readFileSync(stateSentinel, "utf8"), "preserve user state\n");
    console.log(JSON.stringify({ ok: true, manifest: true, install: firstVersion, update: newVersion,
      old_worker_survived_update: true, new_manager_stopped_old_worker: true,
      development_target_untouched: true, uninstall_preserved_user_state: true, engine_log_lines: logs.length }));
  } finally {
    if (!stopped) {
      try { manager(sourceManager, "x.stop('hub-sleeper')\nfor _ in range(80):\n if not m.owned_worker(m.read_json(x.job_path('hub-sleeper'),{})): break\n time.sleep(.1)\nassert not m.owned_worker(m.read_json(x.job_path('hub-sleeper'),{}))"); }
      catch (error) {
        console.error("Owned worker cleanup failed; preserving state at", root, error.message);
        throw error;
      }
    }
    fs.rmSync(root, { recursive: true, force: true });
  }
}
main().catch(error => { console.error(error.stack); process.exitCode = 1; });
