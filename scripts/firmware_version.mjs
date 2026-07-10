#!/usr/bin/env node

import { readFile } from 'node:fs/promises';
import path from 'node:path';
import process from 'node:process';
import { fileURLToPath } from 'node:url';

const repoRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const header = await readFile(
  path.join(repoRoot, 'wiced/apps/rewair/local_bridge/rewair_version.h'),
  'utf8',
);
const match = header.match(/^#define REWAIR_FW_VERSION_NUMBER "(\d+\.\d+\.\d+)"$/m);
if (!match) throw new Error('REWAIR_FW_VERSION_NUMBER is missing or is not semantic x.y.z');

const base = match[1];
const releaseTag = process.argv[2] || '';
if (releaseTag && releaseTag !== `v${base}`) {
  throw new Error(`release tag ${releaseTag} does not match firmware version v${base}`);
}

const version = releaseTag ? base : `${base}-dev`;
process.stdout.write(version);
