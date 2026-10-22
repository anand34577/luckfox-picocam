import { Alert, Button, Card, Center, Loader, PasswordInput, Stack, Text, Title } from '@mantine/core';
import { IconLock } from '@tabler/icons-react';
import { ReactNode, useCallback, useEffect, useState } from 'react';

interface Session {
  auth_required: boolean;
  authenticated: boolean;
  role?: 'admin' | 'read';
}

/** Shows a login form when the server runs with an API key; otherwise renders the dashboard straight away. */
export function AuthGate({ children }: { children: ReactNode }) {
  const [session, setSession] = useState<Session | null>(null);
  const [key, setKey] = useState('');
  const [error, setError] = useState('');
  const [busy, setBusy] = useState(false);

  const refresh = useCallback(() => {
    fetch('/api/v1/session')
      .then((r) => r.json() as Promise<Session>)
      .then(setSession)
      .catch(() => setSession({ auth_required: false, authenticated: true }));
  }, []);
  useEffect(refresh, [refresh]);

  // an expired session shows up as a 401 anywhere in the app: send the user back to the form
  useEffect(() => {
    const orig = window.fetch;
    window.fetch = async (...args) => {
      const r = await orig(...args);
      if (r.status === 401 && String(args[0]).startsWith('/api/') && !String(args[0]).includes('/login')) refresh();
      return r;
    };
    return () => {
      window.fetch = orig;
    };
  }, [refresh]);

  const login = async () => {
    setBusy(true);
    setError('');
    try {
      const r = await fetch('/api/v1/login', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ key }),
      });
      if (r.ok) {
        setKey('');
        refresh();
      } else {
        const e = await r.json().catch(() => null);
        setError(e?.error?.message ?? `Login failed (${r.status})`);
      }
    } catch {
      setError('Cannot reach the server');
    } finally {
      setBusy(false);
    }
  };

  if (!session)
    return (
      <Center h="100vh">
        <Loader />
      </Center>
    );
  if (!session.auth_required || session.authenticated) return <>{children}</>;

  return (
    <Center h="100vh" p="md">
      <Card withBorder w={380} maw="100%" p="lg">
        <form
          onSubmit={(e) => {
            e.preventDefault();
            if (key && !busy) login();
          }}
        >
          <Stack>
            <Title order={3}>
              <IconLock size={20} style={{ verticalAlign: -3, marginRight: 8 }} />
              EdgeAI Vision
            </Title>
            <Text size="sm" c="dimmed">
              Enter an API key to open the dashboard.
            </Text>
            <PasswordInput label="API key" value={key} onChange={(e) => setKey(e.currentTarget.value)} autoFocus />
            {error && (
              <Alert color="red" variant="light">
                {error}
              </Alert>
            )}
            <Button type="submit" loading={busy} disabled={!key}>
              Sign in
            </Button>
          </Stack>
        </form>
      </Card>
    </Center>
  );
}
