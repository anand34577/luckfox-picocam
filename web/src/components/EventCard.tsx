import { Badge, Card, Group, Image, Stack, Text } from '@mantine/core';
import dayjs from 'dayjs';
import { EventRow, personLabel } from '../api';
import { PlateText } from './PlateText';

/** Compact card used in live feeds; click opens the full-frame viewer. */
export function EventCard({ ev, onOpen }: { ev: EventRow; onOpen: (e: EventRow) => void }) {
  const isFace = ev.type === 'face';
  return (
    <Card padding="xs" radius="md" withBorder style={{ cursor: 'pointer' }} onClick={() => onOpen(ev)}>
      <Group wrap="nowrap" gap="sm" align="center">
        <Image
          src={ev.crop_url || ev.extra_url || ev.frame_url}
          w={isFace ? 56 : 96}
          h={56}
          radius="sm"
          fit={isFace ? 'cover' : 'contain'}
          bg="dark.8"
          fallbackSrc="data:image/svg+xml;utf8,<svg xmlns='http://www.w3.org/2000/svg' width='96' height='56'/>"
        />
        <Stack gap={2} style={{ minWidth: 0, flex: 1 }}>
          {isFace ? (
            <Text fw={600} size="sm">
              {personLabel(ev.person_id, ev.person_name)}
            </Text>
          ) : (
            <PlateText text={ev.plate_text} valid={ev.plate_valid} size="sm" />
          )}
          <Group gap={6}>
            {!isFace && (
              <Badge size="xs" variant="light" color="blue">
                {ev.vehicle_type}
              </Badge>
            )}
            <Text size="xs" c="dimmed" truncate>
              {dayjs(ev.ts_ms).format('HH:mm:ss')} · {ev.device_id}
            </Text>
          </Group>
        </Stack>
      </Group>
    </Card>
  );
}
