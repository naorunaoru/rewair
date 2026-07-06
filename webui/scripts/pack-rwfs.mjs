#!/usr/bin/env node
/*
 * pack-rwfs.mjs -- builds an RWFS packed image for the Rewair web UI.
 *
 * Usage:
 *   node pack-rwfs.mjs <out.rwfs> <file>[:servepath] ...
 *
 * Each input file is gzip-compressed (zlib, level 9) and stored with a
 * deterministic gzip header (mtime + OS byte zeroed) so the same inputs
 * always produce a byte-identical output image. Serve paths default to
 * "/" + basename(file); pass "file:servepath" to override.
 *
 * RWFS layout (little-endian throughout):
 *   header (16 bytes): magic "RWF1", u32 file_count, u32 total_size,
 *                       u32 table_crc32 (CRC32 of the table bytes only)
 *   table (40 bytes/entry): char path[24]; u32 offset; u32 size; u32 crc32;
 *                            u8 gzip; u8 pad[3]
 *   data: concatenated gzipped blobs
 */

import { readFileSync, writeFileSync } from 'node:fs';
import { gzipSync } from 'node:zlib';
import { basename } from 'node:path';

const MAX_FILES = 16;
const MAX_PATH = 24; /* incl. NUL */
const MAX_IMAGE_SIZE = 262144; /* 256 KiB */
const HEADER_SIZE = 16;
const ENTRY_SIZE = 40;

function fail( msg )
{
    console.error( `pack-rwfs: ${msg}` );
    process.exit( 1 );
}

/* Standard zlib/PKZIP CRC32 (polynomial 0xEDB88320, reflected). Used instead
 * of relying on zlib.crc32 (Node >= 20 only) so the packer works on older
 * Node too, and so we control the exact algorithm the C reader must match. */
const CRC_TABLE = ( () =>
{
    const table = new Uint32Array( 256 );
    for ( let n = 0; n < 256; n++ )
    {
        let c = n;
        for ( let k = 0; k < 8; k++ )
        {
            c = ( c & 1 ) ? ( 0xedb88320 ^ ( c >>> 1 ) ) : ( c >>> 1 );
        }
        table[n] = c >>> 0;
    }
    return table;
} )();

function crc32( buf )
{
    let crc = 0xffffffff;
    for ( let i = 0; i < buf.length; i++ )
    {
        crc = CRC_TABLE[ ( crc ^ buf[i] ) & 0xff ] ^ ( crc >>> 8 );
    }
    return ( crc ^ 0xffffffff ) >>> 0;
}

/* Deterministic gzip: gzipSync with level 9 still stamps mtime (bytes 4-7)
 * and OS (byte 9) into the header; zero them explicitly so identical inputs
 * always produce identical compressed output. */
function gzipDeterministic( data )
{
    const gz = gzipSync( data, { level: 9 } );
    gz[4] = 0;
    gz[5] = 0;
    gz[6] = 0;
    gz[7] = 0;
    gz[9] = 0;
    return gz;
}

function parseArgs( argv )
{
    if ( argv.length < 2 )
    {
        fail( 'usage: node pack-rwfs.mjs <out.rwfs> <file>[:servepath] ...' );
    }
    const outPath = argv[0];
    const specs = argv.slice( 1 ).map( ( spec ) =>
    {
        const idx = spec.lastIndexOf( ':' );
        /* Guard against Windows-style drive letters ("C:\...") by requiring
         * the split to leave a non-empty file part and treating a single
         * leading-colon-free spec as "no override". */
        let filePath = spec;
        let servePath = null;
        if ( idx > 0 )
        {
            filePath = spec.slice( 0, idx );
            servePath = spec.slice( idx + 1 );
        }
        if ( !servePath )
        {
            servePath = '/' + basename( filePath );
        }
        return { filePath, servePath };
    } );
    return { outPath, specs };
}

function main()
{
    const { outPath, specs } = parseArgs( process.argv.slice( 2 ) );

    if ( specs.length === 0 )
    {
        fail( 'no input files given' );
    }
    if ( specs.length > MAX_FILES )
    {
        fail( `too many files (${specs.length}), max is ${MAX_FILES}` );
    }

    const entries = [];
    const blobs = [];
    let offset = HEADER_SIZE + specs.length * ENTRY_SIZE;

    for ( const { filePath, servePath } of specs )
    {
        const pathBytes = Buffer.byteLength( servePath, 'utf8' );
        if ( pathBytes > MAX_PATH - 1 )
        {
            fail( `serve path "${servePath}" is ${pathBytes} bytes, max is ${MAX_PATH - 1}` );
        }

        let raw;
        try
        {
            raw = readFileSync( filePath );
        }
        catch ( err )
        {
            fail( `cannot read "${filePath}": ${err.message}` );
        }

        const gz = gzipDeterministic( raw );

        entries.push( {
            path: servePath,
            offset,
            size: gz.length,
            crc32: crc32( gz ),
            gzip: 1,
        } );
        blobs.push( gz );
        offset += gz.length;
    }

    const totalSize = offset;
    if ( totalSize > MAX_IMAGE_SIZE )
    {
        fail( `image size ${totalSize} bytes exceeds max ${MAX_IMAGE_SIZE} bytes` );
    }

    const image = Buffer.alloc( totalSize );

    /* --- table (written first so we can CRC it before touching the header) --- */
    const tableSize = specs.length * ENTRY_SIZE;
    const table = image.subarray( HEADER_SIZE, HEADER_SIZE + tableSize );
    entries.forEach( ( e, i ) =>
    {
        const base = i * ENTRY_SIZE;
        const pathBuf = table.subarray( base, base + MAX_PATH );
        pathBuf.fill( 0 );
        pathBuf.write( e.path, 0, 'utf8' );
        table.writeUInt32LE( e.offset, base + 24 );
        table.writeUInt32LE( e.size, base + 28 );
        table.writeUInt32LE( e.crc32, base + 32 );
        table.writeUInt8( e.gzip, base + 36 );
        table.writeUInt8( 0, base + 37 );
        table.writeUInt8( 0, base + 38 );
        table.writeUInt8( 0, base + 39 );
    } );
    const tableCrc = crc32( table );

    /* --- header --- */
    image.write( 'RWF1', 0, 'ascii' );
    image.writeUInt32LE( specs.length, 4 );
    image.writeUInt32LE( totalSize, 8 );
    image.writeUInt32LE( tableCrc, 12 );

    /* --- blobs --- */
    entries.forEach( ( e, i ) =>
    {
        blobs[i].copy( image, e.offset );
    } );

    writeFileSync( outPath, image );
    console.log( `pack-rwfs: wrote ${outPath} (${totalSize} bytes, ${specs.length} files)` );
}

main();
