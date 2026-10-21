import { Badge, Card, Code, CopyButton, Group, Progress, SimpleGrid, Stack, Table, Text, Title, ActionIcon, Tooltip } from '@mantine/core';
import { IconCheck, IconCopy } from '@tabler/icons-react';
import dayjs from 'dayjs';
import { Device, useDevices } from '../api';

export function DevicesPage() {
  const { devices } = useDevices();
  return (
    <Stack>
      <Title order={3}>Devices</Title>
      {devices.length === 0 && (
        <Text c="dimmed">No device has reported yet. Devices send a heartbeat every 10 seconds.</Text>
      )}
      <SimpleGrid cols={{ base: 1, md: 2 }}>
        {devices.map((d) => (
          <DeviceCard key={d.id} d={d} />
        ))}
      </SimpleGrid>
    </Stack>
  );
}

function DeviceCard({ d }: { d: Device }) {
  const s = d.status ?? {};
  const cmaPct = s.cma_total_kb ? ((s.cma_used_kb ?? 0) / s.cma_total_kb) * 100 : 0;
  return (
    <Card withBorder radius="md">
      <Group justify="space-between" mb="sm">
        <Group gap="xs">
          <Text fw={700}>{d.id}</Text>
          <Badge variant="light" color={d.profile === 'face' ? 'grape' : 'blue'}>
            {d.profile === 'face' ? 'Face ID' : 'ANPR'}
          </Badge>
        </Group>
        <Badge color={d.online ? 'teal' : 'red'} variant="dot">
          {d.online ? 'online' : `offline · ${dayjs(d.last_seen).format('DD MMM HH:mm')}`}
        </Badge>
      </Group>
      <Table withRowBorders={false} verticalSpacing={3}>
        <Table.Tbody>
          <Row k="IP address" v={d.ip} />
          <Row k="Video" v={s.width ? `${s.width}×${s.height} @ ${s.stream_fps?.toFixed(1)} fps` : '—'} />
          <Row k="AI" v={s.ai_fps ? `${s.ai_fps.toFixed(1)} fps · ${s.ai_ms?.toFixed(0)} ms per frame` : '—'} />
          <Row
            k="Media memory"
            v={
              <Group gap="xs" wrap="nowrap">
                <Progress value={cmaPct} w={120} color={cmaPct > 92 ? 'red' : 'teal'} />
                <Text size="xs">
                  {((s.cma_used_kb ?? 0) / 1024).toFixed(1)} / {((s.cma_total_kb ?? 0) / 1024).toFixed(0)} MB
                </Text>
              </Group>
            }
          />
          <Row k="Free RAM" v={s.mem_avail_kb ? `${(s.mem_avail_kb / 1024).toFixed(1)} MB` : '—'} />
          {s.stats &&
            Object.entries(s.stats).map(([k, v]) => <Row key={k} k={k.replace(/_/g, ' ')} v={String(v)} />)}
        </Table.Tbody>
      </Table>
      <Stack gap={4} mt="sm">
        <Url label="Clean stream" url={s.streams?.clean} />
        <Url label="Overlay stream" url={s.streams?.overlay} />
      </Stack>
    </Card>
  );
}

function Row({ k, v }: { k: string; v: React.ReactNode }) {
  return (
    <Table.Tr>
      <Table.Td c="dimmed" w={140} tt="capitalize">
        {k}
      </Table.Td>
      <Table.Td>{v}</Table.Td>
    </Table.Tr>
  );
}

function Url({ label, url }: { label: string; url?: string }) {
  if (!url) return null;
  return (
    <Group gap="xs" wrap="nowrap">
      <Text size="xs" c="dimmed" w={100}>
        {label}
      </Text>
      <Code>{url}</Code>
      <CopyButton value={url}>
        {({ copied, copy }) => (
          <Tooltip label={copied ? 'Copied' : 'Copy (open in VLC)'}>
            <ActionIcon size="sm" variant="subtle" onClick={copy}>
              {copied ? <IconCheck size={14} /> : <IconCopy size={14} />}
            </ActionIcon>
          </Tooltip>
        )}
      </CopyButton>
    </Group>
  );
}
