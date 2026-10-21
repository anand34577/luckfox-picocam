import { Card, Grid, Group, ScrollArea, SimpleGrid, Stack, Text, ThemeIcon, Title } from '@mantine/core';
import { IconCar, IconId, IconUserPlus, IconUsers } from '@tabler/icons-react';
import { useCallback, useEffect, useState } from 'react';
import { api, EventRow, Stats, useDevices, useLive } from '../api';
import { DeviceStreams } from '../components/DeviceStreams';
import { EventCard } from '../components/EventCard';
import { EventViewer } from '../components/EventViewer';

export function LivePage() {
  const { devices } = useDevices();
  const [events, setEvents] = useState<EventRow[]>([]);
  const [stats, setStats] = useState<Stats | null>(null);
  const [open, setOpen] = useState<EventRow | null>(null);

  const refreshStats = useCallback(() => api<Stats>('/api/v1/stats').then(setStats).catch(() => {}), []);
  useEffect(() => {
    api<EventRow[]>('/api/v1/events?limit=30').then(setEvents).catch(() => {});
    refreshStats();
  }, [refreshStats]);
  useLive((m) => {
    if (m.kind !== 'event') return;
    setEvents((es) => [m.data, ...es].slice(0, 60));
    refreshStats();
  });

  return (
    <Stack>
      <SimpleGrid cols={{ base: 2, md: 4 }}>
        <Stat icon={<IconCar size={18} />} color="blue" label="Vehicles today" value={stats?.vehicles_today} />
        <Stat icon={<IconId size={18} />} color="yellow" label="Plates read today" value={stats?.plates_today} />
        <Stat icon={<IconUsers size={18} />} color="grape" label="Known people" value={stats?.persons_total} />
        <Stat icon={<IconUserPlus size={18} />} color="teal" label="New faces today" value={stats?.new_persons_today} />
      </SimpleGrid>
      <Grid>
        <Grid.Col span={{ base: 12, lg: 8.5 }}>
          <Stack>
            {devices.length === 0 && (
              <Card withBorder p="xl">
                <Text c="dimmed" ta="center">
                  No devices yet. Start edgeai on a board with this server's address in its config.
                </Text>
              </Card>
            )}
            {devices.map((d) => (
              <DeviceStreams key={d.id} device={d} />
            ))}
          </Stack>
        </Grid.Col>
        <Grid.Col span={{ base: 12, lg: 3.5 }}>
          <Card withBorder radius="md" p="sm">
            <Title order={5} mb="xs">
              Live detections
            </Title>
            <ScrollArea h="calc(100vh - 260px)" offsetScrollbars>
              <Stack gap="xs">
                {events.length === 0 && (
                  <Text size="sm" c="dimmed">
                    Waiting for the first detection…
                  </Text>
                )}
                {events.map((e) => (
                  <EventCard key={e.id} ev={e} onOpen={setOpen} />
                ))}
              </Stack>
            </ScrollArea>
          </Card>
        </Grid.Col>
      </Grid>
      <EventViewer ev={open} onClose={() => setOpen(null)} />
    </Stack>
  );
}

function Stat({ icon, color, label, value }: { icon: React.ReactNode; color: string; label: string; value?: number }) {
  return (
    <Card withBorder radius="md" p="sm">
      <Group gap="sm">
        <ThemeIcon variant="light" color={color} size="lg" radius="md">
          {icon}
        </ThemeIcon>
        <div>
          <Text size="xs" c="dimmed">
            {label}
          </Text>
          <Text fw={700} size="xl">
            {value ?? '–'}
          </Text>
        </div>
      </Group>
    </Card>
  );
}
