import { defineConfig } from 'vite';
import preact from '@preact/preset-vite';

// `npm run dev` proxies the API to a camera: CAMERA=http://172.32.0.93 npm run dev
const camera = process.env.CAMERA || 'http://172.32.0.93';

export default defineConfig({
  plugins: [preact()],
  build: { target: 'es2020', cssCodeSplit: false, assetsInlineLimit: 0, chunkSizeWarningLimit: 400 },
  server: {
    proxy: {
      '/api': camera,
      '/snapshot.jpg': camera,
      '/ws': { target: camera.replace('http', 'ws'), ws: true },
    },
  },
});
