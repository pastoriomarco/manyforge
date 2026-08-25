import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';

export default defineConfig(() => {
  const backend = process.env.MANYFORGE_COMPOSER_PROXY_TARGET
    ?? 'http://localhost:9000';
  return {
    plugins: [react()],
    server: {
      port: 3000,
      proxy: {
        '/api': backend,
        '/ws': { target: backend, ws: true },
      },
    },
    build: {
      outDir: 'dist',
    },
  };
});
