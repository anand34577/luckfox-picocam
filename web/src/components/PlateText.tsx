import { Text, Tooltip } from '@mantine/core';

/** Number plate rendered like a plate: monospace, yellow-ish for valid reads, dashed when unread. */
export function PlateText({ text, valid, size = 'md' }: { text?: string; valid?: boolean; size?: 'sm' | 'md' | 'lg' | 'xl' }) {
  if (!text) {
    return (
      <Tooltip label="Plate detected but not read (no OCR model yet), or no plate visible">
        <Text size={size} c="dimmed" ff="JetBrains Mono, monospace" fs="italic">
          — not read —
        </Text>
      </Tooltip>
    );
  }
  return (
    <Text
      size={size}
      fw={700}
      ff="JetBrains Mono, monospace"
      span
      px={6}
      style={{
        letterSpacing: 1,
        borderRadius: 4,
        border: `1px solid ${valid ? 'var(--mantine-color-yellow-6)' : 'var(--mantine-color-dark-3)'}`,
        background: valid ? 'rgba(250, 204, 21, 0.12)' : 'transparent',
        width: 'fit-content',
      }}
    >
      {text}
    </Text>
  );
}
