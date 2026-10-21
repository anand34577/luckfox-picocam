import {
  ActionIcon, Avatar, Badge, Button, Card, Drawer, Group, Image, Menu, NumberInput, SimpleGrid, Stack, Text, TextInput, Title,
} from '@mantine/core';
import { notifications } from '@mantine/notifications';
import { IconDots, IconGitMerge, IconPencil, IconSearch, IconTrash } from '@tabler/icons-react';
import dayjs from 'dayjs';
import relativeTime from 'dayjs/plugin/relativeTime';
import { useCallback, useEffect, useState } from 'react';
import { api, EventRow, Person, personLabel, useLive } from '../api';
import { EventViewer } from '../components/EventViewer';

dayjs.extend(relativeTime);

export function PeoplePage() {
  const [people, setPeople] = useState<Person[]>([]);
  const [q, setQ] = useState('');
  const [sel, setSel] = useState<Person | null>(null);

  const load = useCallback(() => api<Person[]>(`/api/v1/persons?q=${encodeURIComponent(q)}`).then(setPeople).catch(() => {}), [q]);
  useEffect(() => {
    const t = window.setTimeout(load, 250);
    return () => window.clearTimeout(t);
  }, [load]);
  useLive((m) => {
    if (m.kind === 'event' && m.data.type === 'face') load();
  });

  return (
    <Stack>
      <Group justify="space-between">
        <div>
          <Title order={3}>People</Title>
          <Text size="sm" c="dimmed">
            Every unique face gets a stable ID; the same person keeps it when seen again.
          </Text>
        </div>
        <TextInput placeholder="Search name or ID" leftSection={<IconSearch size={16} />} value={q} onChange={(e) => setQ(e.currentTarget.value)} w={240} />
      </Group>
      <SimpleGrid cols={{ base: 2, sm: 3, md: 4, lg: 6 }}>
        {people.map((p) => (
          <Card key={p.id} withBorder radius="md" padding="sm" style={{ cursor: 'pointer' }} onClick={() => setSel(p)}>
            <Card.Section>
              <Image src={p.cover_url} h={160} fit="cover" bg="dark.8" fallbackSrc="data:image/svg+xml;utf8,<svg xmlns='http://www.w3.org/2000/svg'/>" />
            </Card.Section>
            <Group justify="space-between" mt="xs" wrap="nowrap">
              <Text fw={600} truncate>
                {personLabel(p.id, p.name)}
              </Text>
              <Badge size="sm" variant="light" color="grape">
                ×{p.visits}
              </Badge>
            </Group>
            <Text size="xs" c="dimmed">
              last seen {dayjs(p.last_seen).fromNow()}
            </Text>
          </Card>
        ))}
      </SimpleGrid>
      {people.length === 0 && (
        <Text c="dimmed" ta="center" p="xl">
          No faces yet.
        </Text>
      )}
      <PersonDrawer person={sel} onClose={() => setSel(null)} onChanged={load} />
    </Stack>
  );
}

function PersonDrawer({ person, onClose, onChanged }: { person: Person | null; onClose: () => void; onChanged: () => void }) {
  const [name, setName] = useState('');
  const [events, setEvents] = useState<EventRow[]>([]);
  const [open, setOpen] = useState<EventRow | null>(null);
  const [mergeFrom, setMergeFrom] = useState<number | string>('');

  useEffect(() => {
    if (!person) return;
    setName(person.name);
    api<EventRow[]>(`/api/v1/events?person_id=${person.id}&limit=100`).then(setEvents).catch(() => {});
  }, [person]);
  if (!person) return null;

  const save = async () => {
    await api(`/api/v1/persons/${person.id}`, { method: 'PATCH', body: JSON.stringify({ name }) });
    notifications.show({ message: 'Name saved', color: 'teal' });
    onChanged();
  };
  const merge = async () => {
    if (!mergeFrom) return;
    try {
      await api(`/api/v1/persons/${person.id}/merge/${mergeFrom}`, { method: 'POST' });
      notifications.show({ message: `ID ${mergeFrom} merged into ID ${person.id}`, color: 'teal' });
      onChanged();
      onClose();
    } catch (e) {
      notifications.show({ message: String(e), color: 'red' });
    }
  };
  const remove = async () => {
    await api(`/api/v1/persons/${person.id}`, { method: 'DELETE' });
    onChanged();
    onClose();
  };

  return (
    <Drawer opened onClose={onClose} position="right" size="lg" title={personLabel(person.id, person.name)}>
      <Stack>
        <Group align="flex-start">
          <Avatar src={person.cover_url} size={96} radius="md" />
          <Stack gap={4} style={{ flex: 1 }}>
            <Text size="sm">
              <b>ID {person.id}</b> · seen {person.visits} times
            </Text>
            <Text size="xs" c="dimmed">
              first {dayjs(person.first_seen).format('DD MMM YYYY HH:mm')} · last {dayjs(person.last_seen).format('DD MMM YYYY HH:mm')}
            </Text>
            <Group gap="xs" mt={4}>
              <TextInput size="xs" placeholder="Give this person a name" value={name} onChange={(e) => setName(e.currentTarget.value)} style={{ flex: 1 }} />
              <Button size="xs" leftSection={<IconPencil size={14} />} onClick={save}>
                Save
              </Button>
              <Menu position="bottom-end">
                <Menu.Target>
                  <ActionIcon variant="default" size="md">
                    <IconDots size={16} />
                  </ActionIcon>
                </Menu.Target>
                <Menu.Dropdown>
                  <Menu.Item color="red" leftSection={<IconTrash size={14} />} onClick={remove}>
                    Delete person
                  </Menu.Item>
                </Menu.Dropdown>
              </Menu>
            </Group>
            <Group gap="xs">
              <NumberInput size="xs" placeholder="Other ID that is the same person" value={mergeFrom} onChange={setMergeFrom} hideControls style={{ flex: 1 }} />
              <Button size="xs" variant="light" leftSection={<IconGitMerge size={14} />} onClick={merge}>
                Merge into this
              </Button>
            </Group>
          </Stack>
        </Group>
        <Title order={6}>Sightings</Title>
        <SimpleGrid cols={3}>
          {events.map((e) => (
            <Card key={e.id} padding={4} withBorder style={{ cursor: 'pointer' }} onClick={() => setOpen(e)}>
              <Image src={e.crop_url} h={110} fit="cover" radius="sm" />
              <Text size="xs" c="dimmed" mt={2}>
                {dayjs(e.ts_ms).format('DD MMM HH:mm:ss')}
              </Text>
              <Text size="xs" c="dimmed" truncate>
                {e.device_id}
              </Text>
            </Card>
          ))}
        </SimpleGrid>
      </Stack>
      <EventViewer ev={open} onClose={() => setOpen(null)} />
    </Drawer>
  );
}
