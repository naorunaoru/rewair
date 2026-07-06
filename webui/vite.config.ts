import { defineConfig } from 'vite';

// Rewair web UI build config.
//
// The device serves this app from a packed RWFS image (see
// scripts/pack-rwfs.mjs) at fixed paths: "/", "/app.js", "/rewair.css".
// There is no server-side routing or hashed-asset lookup on-device, so we
// pin Vite's output filenames instead of letting it hash them, and use
// relative asset URLs (base: './') so index.html works whether it's
// fetched from "/" or opened as a static file.
export default defineConfig({
  base: './',
  build: {
    rollupOptions: {
      output: {
        entryFileNames: 'app.js',
        chunkFileNames: 'app.js',
        assetFileNames: (assetInfo) => {
          const name = assetInfo.name || '';
          if (name.endsWith('.css')) return 'rewair.css';
          return 'assets/[name][extname]';
        },
      },
    },
  },
});
