import { defineConfig } from 'vitest/config';

export default defineConfig({
  test: {
    globals: true,
    testTimeout: 30000,  // 30 seconds for transcription tests
    hookTimeout: 15000,  // 15 seconds for setup/teardown
    include: ['tests/**/*.test.ts'],
    // Run test files sequentially to avoid connection interference
    // (E2E tests share the same server)
    fileParallelism: false,
  },
});
