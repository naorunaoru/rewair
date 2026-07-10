#!/usr/bin/env node

import { createHash } from 'node:crypto';
import { mkdir, copyFile, readFile, writeFile } from 'node:fs/promises';
import path from 'node:path';
import process from 'node:process';

const [
  sdkDir,
  outputDir,
  version = 'dev',
  releaseTag = '',
  sourceRevision = 'unknown',
  sdkRevision = 'unknown',
] = process.argv.slice(2);
if (!sdkDir || !outputDir) {
  console.error('usage: package_release.mjs <sdk-dir> <output-dir> [version] [release-tag] [source-revision] [sdk-revision]');
  process.exit(1);
}

const buildName = 'rewair_local_bridge-AWAIR-FreeRTOS-LwIP-SDIO';
const sources = {
  bootloader: path.join(sdkDir, 'build/waf_bootloader-NoOS-NoNS-AWAIR-SDIO/binary/waf_bootloader-NoOS-NoNS-AWAIR-SDIO.bin'),
  dct: path.join(sdkDir, `build/${buildName}/DCT.bin`),
  application: path.join(sdkDir, `build/${buildName}/binary/${buildName}.bin`),
  appsLut: path.join(sdkDir, `build/${buildName}/APPS.bin`),
  wifiFirmware: path.join(sdkDir, 'resources/firmware/43362/43362A2.bin'),
  sflashLoader: path.join(sdkDir, 'build/waf_sflash_write-NoOS-NoNS-AWAIR/binary/waf_sflash_write-NoOS-NoNS-AWAIR.bin'),
};

await mkdir(outputDir, { recursive: true });

const sha256 = (data) => createHash('sha256').update(data).digest('hex');
const hex = (value) => `0x${value.toString(16).padStart(8, '0')}`;
const assets = [];

function validateCortexMImage(name, data, address) {
  if (data.length < 8) throw new Error(`${name} is too small to contain a vector table`);
  const stack = data.readUInt32LE(0);
  const reset = data.readUInt32LE(4);
  if (stack < 0x20000000 || stack > 0x20020000) throw new Error(`${name} has invalid initial SP ${hex(stack)}`);
  if ((reset & 1) === 0 || (reset & ~1) < address || (reset & ~1) >= address + data.length) {
    throw new Error(`${name} has invalid reset vector ${hex(reset)}`);
  }
}

async function addAsset({ id, filename, source, memory, address, required, destructive = false, description, extra = {} }) {
  const data = await readFile(source);
  await copyFile(source, path.join(outputDir, filename));
  const asset = {
    id,
    filename,
    memory,
    address: hex(address),
    size: data.length,
    sha256: sha256(data),
    required,
    destructive,
    description,
    ...extra,
  };
  assets.push(asset);
  return { data, asset };
}

const bootloader = await addAsset({
  id: 'bootloader', filename: 'rewair-bootloader.bin', source: sources.bootloader,
  memory: 'internal-flash', address: 0x08000000, required: true,
  description: 'WICED bootloader with Rewair OTA copy and rollback support.',
});
validateCortexMImage('bootloader', bootloader.data, 0x08000000);
const application = await addAsset({
  id: 'application', filename: 'rewair-application.bin', source: sources.application,
  memory: 'internal-flash', address: 0x0800c000, required: true,
  description: 'Rewair application firmware with the web UI embedded.',
});
validateCortexMImage('application', application.data, 0x0800c000);
const embeddedVersion = `rewair ${version}`;
if (!application.data.includes(Buffer.from(`${embeddedVersion}\0`, 'ascii'))) {
  throw new Error(`application does not contain expected firmware version ${embeddedVersion}`);
}
const loaderData = await readFile(sources.sflashLoader);
const loaderEntryPoint = loaderData.readUInt32LE(0);
const loaderStackAddress = loaderData.readUInt32LE(4);
const loaderBufferSize = loaderData.readUInt32LE(8);
if (loaderBufferSize < 4096 || loaderBufferSize > 0x10000) {
  throw new Error(`SPI-flash loader has invalid buffer size ${loaderBufferSize}`);
}
await addAsset({
  id: 'default-dct', filename: 'rewair-default-dct.bin', source: sources.dct,
  memory: 'internal-flash', address: 0x08004000, required: false, destructive: true,
  description: 'Optional default DCT. Erases saved Wi-Fi/settings and contains a build-generated MAC address; normal installs must preserve the device DCT.',
});
const lut = await addAsset({
  id: 'apps-lut', filename: 'rewair-apps-lut.bin', source: sources.appsLut,
  memory: 'external-spi-flash', address: 0x00101000, required: true,
  description: 'WICED external application lookup table.',
});
const wifi = await addAsset({
  id: 'wifi-firmware', filename: 'rewair-bcm43362a2.bin', source: sources.wifiFirmware,
  memory: 'external-spi-flash', address: 0x00102000, required: true,
  description: 'BCM43362 A2 WLAN firmware.',
});
if (lut.data.length !== 0x1000) throw new Error(`APPS LUT must be 4096 bytes, got ${lut.data.length}`);
const sflash = Buffer.concat([lut.data, wifi.data]);
await writeFile(path.join(outputDir, 'rewair-sflash.bin'), sflash);
assets.push({
  id: 'sflash-combined', filename: 'rewair-sflash.bin', memory: 'external-spi-flash',
  address: hex(0x00101000), size: sflash.length, sha256: sha256(sflash), required: true,
  destructive: false, replaces: ['apps-lut', 'wifi-firmware'],
  description: 'Combined APPS lookup table and BCM43362 A2 firmware for one sequential external-flash write.',
});
await addAsset({
  id: 'sflash-loader', filename: 'rewair-sflash-loader.bin', source: sources.sflashLoader,
  memory: 'ram', address: 0x20000000, required: true,
  description: 'WICED SPI-flash writer loaded into STM32 SRAM by the browser flasher.',
  extra: {
    protocol: {
      configAddress: hex(0x20000000),
      transferAddress: hex(0x2000000c),
      dataAddress: hex(0x2000001c),
      imageEntryPoint: hex(loaderEntryPoint),
      stackAddress: hex(loaderStackAddress),
      bufferSize: loaderBufferSize,
      command: { writeEraseIfNeeded: '0x00000080', postWriteVerify: '0x00000008' },
    },
  },
});

const manifest = {
  schemaVersion: 2,
  product: 'rewair-emw3165',
  version,
  firmwareVersion: embeddedVersion,
  releaseTag: releaseTag || null,
  sourceRevision,
  sdk: {
    repository: 'https://github.com/kamejoko80/wiced-emw3165',
    revision: sdkRevision,
  },
  target: {
    module: 'EMW3165',
    mcu: 'STM32F411CE',
    wlan: 'BCM43362A2',
    externalFlash: 'MX25L1606E',
    externalFlashSize: 0x200000,
  },
  install: {
    normal: ['bootloader', 'application', 'sflash-combined'],
    factoryResetAdds: ['default-dct'],
  },
  assets,
};

await writeFile(path.join(outputDir, 'manifest.json'), `${JSON.stringify(manifest, null, 2)}\n`);

const checksumFiles = [...assets.map((asset) => asset.filename), 'manifest.json'];
const checksums = [];
for (const filename of [...new Set(checksumFiles)].sort()) {
  const data = await readFile(path.join(outputDir, filename));
  checksums.push(`${sha256(data)}  ${filename}`);
}
await writeFile(path.join(outputDir, 'SHA256SUMS.txt'), `${checksums.join('\n')}\n`);

const readme = `Rewair ${version} firmware bundle

Target: EMW3165 / STM32F411CE / BCM43362A2 / MX25L1606E

Normal installation (preserves saved DCT data):
  internal flash  0x08000000  rewair-bootloader.bin
  internal flash  0x0800c000  rewair-application.bin
  external flash  0x00101000  rewair-sflash.bin

rewair-default-dct.bin is intentionally excluded from the normal install. Writing it
at 0x08004000 resets persistent configuration and installs the build-generated MAC
address. Use it only for explicit factory recovery, never across a fleet unchanged.

manifest.json is the authoritative machine-readable description for the GitHub Pages
browser flasher. SHA256SUMS.txt covers every published binary and the manifest.
`;
await writeFile(path.join(outputDir, 'FLASHING.txt'), readme);

console.log(`Packaged ${assets.length} asset records in ${outputDir}`);
