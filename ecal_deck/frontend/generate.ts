import { execSync } from 'node:child_process';
import { copyFileSync, mkdirSync, existsSync, readFileSync } from 'node:fs';
import { platform } from 'node:os';
import { join } from 'node:path';

// On Windows, node_modules/.bin contains .cmd wrappers; on Unix no extension is needed.
const ext    = platform() === 'win32' ? '.cmd' : '';
const plugin = join('node_modules', '.bin', `protoc-gen-ts_proto${ext}`);
const proto  = join('..', 'proto');
const out    = join('src', 'generated');

mkdirSync(out, { recursive: true });

// Canonical source of truth is SUMO's src/libsumo/sumo_ecal.proto — the same
// file that libsumocpp.so embeds.  When SUMO_HOME is set we compare it against
// the local proto/sumo.proto.  The local copy stays git-tracked (fallback for
// Windows boxes that build only the frontend, and to bootstrap a fresh clone).
//
// Behavior on mismatch:
//   - local missing       → copy from upstream
//   - identical           → no-op
//   - differ              → ERROR with diff-summary, telling the user either
//                           to update their SUMO clone (if upstream is stale)
//                           or rerun with SYNC_SUMO_PROTO=1 to overwrite local.
// This prevents silent downgrades when SUMO_HOME points at an out-of-date
// SUMO checkout that hasn't pulled the latest sumo_ecal.proto.
const sumoHome   = process.env.SUMO_HOME;
const localProto = join(proto, 'sumo.proto');
if (sumoHome) {
  const upstream = join(sumoHome, 'src', 'libsumo', 'sumo_ecal.proto');
  if (existsSync(upstream)) {
    const a = readFileSync(upstream);
    const haveLocal = existsSync(localProto);
    const b = haveLocal ? readFileSync(localProto) : Buffer.alloc(0);
    if (!haveLocal) {
      console.log(`syncing ${localProto} <- ${upstream} (local missing)`);
      copyFileSync(upstream, localProto);
    } else if (!a.equals(b)) {
      if (process.env.SYNC_SUMO_PROTO === '1') {
        console.log(`syncing ${localProto} <- ${upstream} (SYNC_SUMO_PROTO=1)`);
        copyFileSync(upstream, localProto);
      } else {
        console.error(
          `\nERROR: ${localProto} differs from ${upstream}\n` +
          `  Local  : ${b.length} bytes\n` +
          `  Upstream: ${a.length} bytes\n\n` +
          `The canonical schema lives in SUMO at src/libsumo/sumo_ecal.proto.\n` +
          `Either:\n` +
          `  (a) Update your SUMO clone (git pull in ${sumoHome}) if upstream is stale, or\n` +
          `  (b) Rerun with SYNC_SUMO_PROTO=1 to overwrite the local copy from upstream.\n` +
          `Refusing to silently overwrite ${localProto}.\n`
        );
        process.exit(1);
      }
    }
  }
}

const tsCmd = [
  'protoc',
  `--plugin=${plugin}`,
  `--ts_proto_out=${out}`,
  '--ts_proto_opt=onlyTypes=false',
  '--ts_proto_opt=snakeToCamel=false',
  `-I ${proto}`,
  localProto,
].join(' ');

console.log(tsCmd);
execSync(tsCmd, { stdio: 'inherit' });

const pyCmd = [
  'protoc',
  `--python_out=${proto}`,
  `-I ${proto}`,
  localProto,
].join(' ');

console.log(pyCmd);
execSync(pyCmd, { stdio: 'inherit' });
