import { Badge, Card, Group, SegmentedControl, SimpleGrid, Stack, Text } from '@mantine/core';
import { IconCar, IconUserScan } from '@tabler/icons-react';
import { useState } from 'react';
import { Device } from '../api';
import { WebRTCPlayer } from './WebRTCPlayer';

type View = 'both' | 'overlay' | 'clean';

/** A device's live video: clean and overlay streams side by side, or one of them. */
export function DeviceStreams({ device }: { device: Device }) {
  const [view, setView] = useState<View>('both');
  const st = device.status ?? {};
  const ratio = st.width && st.height ? st.width / st.height : 16 / 9;
  const Icon = device.profile === 'face' ? IconUserScan : IconCar;
  return (
    <Card withBorder radius="md" padding="md">
      <Stack gap="sm">
        <Group justify="space-between" wrap="wrap">
          <Group gap="xs">
            <Icon size={20} />
            <Text fw={600}>{device.id}</Text>
            <Badge color={device.online ? 'teal' : 'red'} variant="dot">
              {device.online ? 'online' : 'offline'}
            </Badge>
            <Badge variant="light" color={device.profile === 'face' ? 'grape' : 'blue'}>
              {device.profile === 'face' ? 'Face ID' : 'ANPR'}
            </Badge>
            {device.online && (
              <Text size="xs" c="dimmed">
                {st.stream_fps?.toFixed(0)} fps · AI {st.ai_fps?.toFixed(1)} fps
              </Text>
            )}
          </Group>
          <SegmentedControl
            size="xs"
            value={view}
            onChange={(v) => setView(v as View)}
            data={[
              { label: 'Side by side', value: 'both' },
              { label: 'With boxes', value: 'overlay' },
              { label: 'Clean', value: 'clean' },
            ]}
          />
        </Group>
        <SimpleGrid cols={{ base: 1, md: view === 'both' ? 2 : 1 }}>
          {view !== 'clean' && (
            <Stack gap={4}>
              {view === 'both' && <Text size="xs" c="dimmed">With bounding boxes</Text>}
              <WebRTCPlayer path={device.overlay_path} ratio={ratio} />
            </Stack>
          )}
          {view !== 'overlay' && (
            <Stack gap={4}>
              {view === 'both' && <Text size="xs" c="dimmed">Clean</Text>}
              <WebRTCPlayer path={device.clean_path} ratio={ratio} />
            </Stack>
          )}
        </SimpleGrid>
      </Stack>
    </Card>
  );
}
