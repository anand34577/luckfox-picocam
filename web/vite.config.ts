import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

// `npm run dev` proxies the API to a running edgeai-server
const server = process.env.EDGEAI_SERVER ?? 'http://localhost:8090';

export default defineConfig({
  plugins: [react()],
  server: {
    host: true,
    proxy: {
      '/api/v1/ws': { target: server.replace('http', 'ws'), ws: true },
      '/api': server,
      '/images': server,
    },
  },
});
