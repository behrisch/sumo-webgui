import { execSync } from 'node:child_process';
import { mkdirSync } from 'node:fs';
import { platform } from 'node:os';
import { join } from 'node:path';

// On Windows, node_modules/.bin contains .cmd wrappers; on Unix no extension is needed.
const ext    = platform() === 'win32' ? '.cmd' : '';
const plugin = join('node_modules', '.bin', `protoc-gen-ts_proto${ext}`);
const proto  = join('..', 'proto');
const out    = join('src', 'generated');

mkdirSync(out, { recursive: true });

const tsCmd = [
  'protoc',
  `--plugin=${plugin}`,
  `--ts_proto_out=${out}`,
  '--ts_proto_opt=onlyTypes=false',
  '--ts_proto_opt=snakeToCamel=false',
  `-I ${proto}`,
  join(proto, 'sumo.proto'),
].join(' ');

console.log(tsCmd);
execSync(tsCmd, { stdio: 'inherit' });

const pyCmd = [
  'protoc',
  `--python_out=${proto}`,
  `-I ${proto}`,
  join(proto, 'sumo.proto'),
].join(' ');

console.log(pyCmd);
execSync(pyCmd, { stdio: 'inherit' });
