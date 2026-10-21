import { Badge, Button, Card, Group, Image, Select, Stack, Table, Text, TextInput, Title } from '@mantine/core';
import { IconSearch } from '@tabler/icons-react';
import dayjs from 'dayjs';
import { useCallback, useEffect, useState } from 'react';
import { api, EventRow, useLive } from '../api';
import { EventViewer } from '../components/EventViewer';
import { PlateText } from '../components/PlateText';

export function VehiclesPage() {
  const [rows, setRows] = useState<EventRow[]>([]);
  const [plate, setPlate] = useState('');
  const [vtype, setVtype] = useState<string | null>(null);
  const [open, setOpen] = useState<EventRow | null>(null);
  const [more, setMore] = useState(true);

  const load = useCallback(
    async (before?: number) => {
      const q = new URLSearchParams({ type: 'vehicle', limit: '50' });
      if (plate) q.set('plate', plate);
      if (before) q.set('before', String(before));
      const r = await api<EventRow[]>(`/api/v1/events?${q}`);
      setMore(r.length === 50);
      setRows((old) => (before ? [...old, ...r] : r));
    },
    [plate],
  );
  useEffect(() => {
    const t = window.setTimeout(() => load().catch(() => {}), 250);
    return () => window.clearTimeout(t);
  }, [load]);
  useLive((m) => {
    if (m.kind === 'event' && m.data.type === 'vehicle' && !plate) setRows((r) => [m.data, ...r]);
  });

  const shown = vtype ? rows.filter((r) => r.vehicle_type === vtype) : rows;
  const types = Array.from(new Set(rows.map((r) => r.vehicle_type).filter(Boolean))) as string[];

  return (
    <Stack>
      <Group justify="space-between">
        <Title order={3}>Vehicles & number plates</Title>
        <Group>
          <Select placeholder="All vehicle types" data={types} value={vtype} onChange={setVtype} clearable w={180} />
          <TextInput
            placeholder="Search plate, e.g. MH12"
            leftSection={<IconSearch size={16} />}
            value={plate}
            onChange={(e) => setPlate(e.currentTarget.value.toUpperCase())}
            w={240}
          />
        </Group>
      </Group>
      <Card withBorder radius="md" p={0}>
        <Table highlightOnHover verticalSpacing="xs" horizontalSpacing="md">
          <Table.Thead>
            <Table.Tr>
              <Table.Th>Plate crop</Table.Th>
              <Table.Th>Number</Table.Th>
              <Table.Th>Vehicle</Table.Th>
              <Table.Th>Snapshot</Table.Th>
              <Table.Th>Time</Table.Th>
              <Table.Th>Device</Table.Th>
            </Table.Tr>
          </Table.Thead>
          <Table.Tbody>
            {shown.map((r) => (
              <Table.Tr key={r.id} style={{ cursor: 'pointer' }} onClick={() => setOpen(r)}>
                <Table.Td>
                  {r.crop_url ? (
                    <Image src={r.crop_url} h={40} w={120} fit="contain" radius="sm" bg="dark.8" />
                  ) : (
                    <Text size="xs" c="dimmed">no plate</Text>
                  )}
                </Table.Td>
                <Table.Td>
                  <PlateText text={r.plate_text} valid={r.plate_valid} />
                </Table.Td>
                <Table.Td>
                  <Badge variant="light">{r.vehicle_type}</Badge>
                </Table.Td>
                <Table.Td>
                  {r.frame_url && <Image src={r.frame_url} h={48} w={86} radius="sm" fit="cover" />}
                </Table.Td>
                <Table.Td>
                  <Text size="sm">{dayjs(r.ts_ms).format('DD MMM, HH:mm:ss')}</Text>
                </Table.Td>
                <Table.Td>
                  <Text size="sm" c="dimmed">{r.device_id}</Text>
                </Table.Td>
              </Table.Tr>
            ))}
          </Table.Tbody>
        </Table>
        {shown.length === 0 && (
          <Text c="dimmed" ta="center" p="xl">
            No vehicles yet.
          </Text>
        )}
      </Card>
      {more && rows.length > 0 && (
        <Button variant="subtle" onClick={() => load(rows[rows.length - 1].id)}>
          Load more
        </Button>
      )}
      <EventViewer ev={open} onClose={() => setOpen(null)} />
    </Stack>
  );
}
