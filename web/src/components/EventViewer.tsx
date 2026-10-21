import { Badge, Group, Image, Modal, SimpleGrid, Stack, Table, Text } from '@mantine/core';
import dayjs from 'dayjs';
import { EventRow, personLabel } from '../api';
import { PlateText } from './PlateText';

/** Full-frame image with the box drawn over it, crops and details. */
export function EventViewer({ ev, onClose }: { ev: EventRow | null; onClose: () => void }) {
  if (!ev) return null;
  const m = (ev.meta ?? {}) as Record<string, unknown>;
  const fw = (m.frame_w as number) || 960;
  const fh = (m.frame_h as number) || 540;
  const boxes: { b: number[]; color: string }[] = [];
  if (Array.isArray(m.vehicle_box)) boxes.push({ b: m.vehicle_box as number[], color: '#40c057' });
  if (Array.isArray(m.plate_box)) boxes.push({ b: m.plate_box as number[], color: '#fcc419' });
  if (Array.isArray(m.face_box)) boxes.push({ b: m.face_box as number[], color: '#40c057' });

  return (
    <Modal opened onClose={onClose} size="xl" title={ev.type === 'face' ? personLabel(ev.person_id, ev.person_name) : 'Vehicle'} centered>
      <Stack>
        <div style={{ position: 'relative', borderRadius: 8, overflow: 'hidden', background: '#000' }}>
          {ev.frame_url ? (
            <img src={ev.frame_url} style={{ width: '100%', display: 'block' }} />
          ) : (
            <Text p="xl" c="dimmed" ta="center">
              No full-frame snapshot (device was short on memory)
            </Text>
          )}
          {ev.frame_url && (
            <svg viewBox={`0 0 ${fw} ${fh}`} style={{ position: 'absolute', inset: 0, width: '100%', height: '100%' }}>
              {boxes.map((x, i) => (
                <rect key={i} x={x.b[0]} y={x.b[1]} width={x.b[2] - x.b[0]} height={x.b[3] - x.b[1]} fill="none" stroke={x.color} strokeWidth={3} />
              ))}
            </svg>
          )}
        </div>
        <SimpleGrid cols={{ base: 1, sm: 2 }}>
          <Group align="flex-start">
            {ev.crop_url && <Image src={ev.crop_url} h={ev.type === 'face' ? 140 : 80} w="auto" radius="sm" />}
            {ev.extra_url && <Image src={ev.extra_url} h={140} w="auto" radius="sm" />}
          </Group>
          <Table withRowBorders={false} verticalSpacing={4}>
            <Table.Tbody>
              {ev.type === 'vehicle' ? (
                <>
                  <Row k="Plate" v={<PlateText text={ev.plate_text} valid={ev.plate_valid} />} />
                  <Row k="Vehicle" v={<Badge variant="light">{ev.vehicle_type}</Badge>} />
                  <Row k="Plate format" v={ev.plate_text ? (ev.plate_valid ? 'Valid Indian format' : 'Unverified') : '—'} />
                </>
              ) : (
                <>
                  <Row k="Person" v={personLabel(ev.person_id, ev.person_name)} />
                  <Row k="Face quality" v={(ev.quality ?? 0).toFixed(2)} />
                </>
              )}
              <Row k="Time" v={dayjs(ev.ts_ms).format('DD MMM YYYY, HH:mm:ss')} />
              <Row k="Device" v={ev.device_id} />
              <Row k="Track" v={`#${ev.track_id}`} />
            </Table.Tbody>
          </Table>
        </SimpleGrid>
      </Stack>
    </Modal>
  );
}

function Row({ k, v }: { k: string; v: React.ReactNode }) {
  return (
    <Table.Tr>
      <Table.Td c="dimmed" w={120}>
        {k}
      </Table.Td>
      <Table.Td>{v}</Table.Td>
    </Table.Tr>
  );
}
