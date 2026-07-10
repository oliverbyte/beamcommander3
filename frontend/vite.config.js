import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'

// https://vite.dev/config/
export default defineConfig({
  plugins: [vue()],
  server: {
    host: true, // listen on 0.0.0.0 (all network interfaces), not just localhost
    proxy: {
      '/api':      { target: 'http://localhost:8000', changeOrigin: true },
      '/laser':    { target: 'http://localhost:8000', changeOrigin: true },
      '/blackout':   { target: 'http://localhost:8000', changeOrigin: true },
      '/brightness': { target: 'http://localhost:8000', changeOrigin: true },
      '/flash':      { target: 'http://localhost:8000', changeOrigin: true },
      '/mirror':     { target: 'http://localhost:8000', changeOrigin: true },
      '/motion':     { target: 'http://localhost:8000', changeOrigin: true },
      '/rotation':   { target: 'http://localhost:8000', changeOrigin: true },
      '/move':       { target: 'http://localhost:8000', changeOrigin: true },
      '/ws':         { target: 'ws://localhost:8000', ws: true },
    },
  },
})
