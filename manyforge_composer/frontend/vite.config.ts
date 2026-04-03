import { defineConfig } from 'vite';
import react from '@vitejs/plugin-react';
import tailwindcss from '@tailwindcss/vite';

export default defineConfig({
  plugins: [react(), tailwindcss()],
  server: {
    port: 3000,
    proxy: {
      '/api': 'http://localhost:9000',
      '/ws': {
        target: 'ws://localhost:9000',
        ws: true,
      },
    },
  },
  build: {
    outDir: 'dist',
  },
});
