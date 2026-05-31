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
// file that libsumocpp.so embeds.  When SUMO_HOME is set we sync the local
// proto/sumo.proto from there so the schema cannot drift; the local copy is
// kept git-tracked as a fallback for environments without SUMO checked out
// (e.g. Windows boxes that only build the frontend).
const sumoHome   = process.env.SUMO_HOME;
const localProto = join(proto, 'sumo.proto');
if (sumoHome) {
  const upstream = join(sumoHome, 'src', 'libsumo', 'sumo_ecal.proto');
  if (existsSync(upstream)) {
    const a = readFileSync(upstream);
    const b = existsSync(localProto) ? readFileSync(localProto) : Buffer.alloc(0);
    if (!a.equals(b)) {
      console.log(`syncing ${localProto} <- ${upstream}`);
      copyFileSync(upstream, localProto);
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
