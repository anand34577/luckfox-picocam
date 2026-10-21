import { ActionIcon, AppShell, Badge, Burger, Group, NavLink, Text, ThemeIcon, Tooltip, useMantineColorScheme } from '@mantine/core';
import { useDisclosure } from '@mantine/hooks';
import { IconCar, IconDeviceCctv, IconEye, IconMoon, IconServer, IconSun, IconUsers } from '@tabler/icons-react';
import { Link, Navigate, Route, Routes, useLocation } from 'react-router-dom';
import { useLive } from './api';
import { DevicesPage } from './pages/Devices';
import { LivePage } from './pages/Live';
import { PeoplePage } from './pages/People';
import { VehiclesPage } from './pages/Vehicles';

const nav = [
  { to: '/live', label: 'Live', icon: IconDeviceCctv },
  { to: '/vehicles', label: 'Vehicles & ANPR', icon: IconCar },
  { to: '/people', label: 'People', icon: IconUsers },
  { to: '/devices', label: 'Devices', icon: IconServer },
];

export function App() {
  const [opened, { toggle, close }] = useDisclosure();
  const loc = useLocation();
  const connected = useLive(() => {});
  const { colorScheme, toggleColorScheme } = useMantineColorScheme();

  return (
    <AppShell header={{ height: 56 }} navbar={{ width: 220, breakpoint: 'sm', collapsed: { mobile: !opened } }} padding="md">
      <AppShell.Header>
        <Group h="100%" px="md" justify="space-between">
          <Group gap="xs">
            <Burger opened={opened} onClick={toggle} hiddenFrom="sm" size="sm" />
            <ThemeIcon variant="gradient" gradient={{ from: 'teal', to: 'cyan' }} radius="md">
              <IconEye size={18} />
            </ThemeIcon>
            <Text fw={700}>EdgeAI Vision</Text>
          </Group>
          <Group gap="xs">
            <Tooltip label={connected ? 'Live updates connected' : 'Reconnecting to server…'}>
              <Badge variant="dot" color={connected ? 'teal' : 'red'}>
                {connected ? 'live' : 'offline'}
              </Badge>
            </Tooltip>
            <ActionIcon variant="default" onClick={toggleColorScheme} aria-label="Toggle theme">
              {colorScheme === 'dark' ? <IconSun size={16} /> : <IconMoon size={16} />}
            </ActionIcon>
          </Group>
        </Group>
      </AppShell.Header>
      <AppShell.Navbar p="xs">
        {nav.map((n) => (
          <NavLink
            key={n.to}
            component={Link}
            to={n.to}
            label={n.label}
            leftSection={<n.icon size={18} />}
            active={loc.pathname.startsWith(n.to)}
            onClick={close}
            style={{ borderRadius: 8 }}
          />
        ))}
      </AppShell.Navbar>
      <AppShell.Main>
        <Routes>
          <Route path="/live" element={<LivePage />} />
          <Route path="/vehicles" element={<VehiclesPage />} />
          <Route path="/people" element={<PeoplePage />} />
          <Route path="/devices" element={<DevicesPage />} />
          <Route path="*" element={<Navigate to="/live" replace />} />
        </Routes>
      </AppShell.Main>
    </AppShell>
  );
}
