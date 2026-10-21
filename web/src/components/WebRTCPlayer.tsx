import { useEffect, useRef, useState } from 'react';
import { Box, Center, Loader, Text } from '@mantine/core';
import { IconVideoOff } from '@tabler/icons-react';
import { getWebrtcBase } from '../api';

type State = 'connecting' | 'playing' | 'error';

/** Plays a MediaMTX path over WebRTC (WHEP). Reconnects if the stream drops. */
export function WebRTCPlayer({ path, ratio = 16 / 9 }: { path: string; ratio?: number }) {
  const video = useRef<HTMLVideoElement>(null);
  const [state, setState] = useState<State>('connecting');

  useEffect(() => {
    let pc: RTCPeerConnection | null = null;
    let retry: number | undefined;
    let closed = false;

    const start = async () => {
      setState('connecting');
      try {
        const base = await getWebrtcBase();
        pc = new RTCPeerConnection();
        pc.addTransceiver('video', { direction: 'recvonly' });
        pc.ontrack = (e) => {
          if (video.current) video.current.srcObject = e.streams[0];
        };
        pc.onconnectionstatechange = () => {
          const s = pc?.connectionState;
          if (s === 'connected') setState('playing');
          if (s === 'failed' || s === 'disconnected') again();
        };
        const offer = await pc.createOffer();
        await pc.setLocalDescription(offer);
        const r = await fetch(`${base}/${path}/whep`, {
          method: 'POST',
          headers: { 'Content-Type': 'application/sdp' },
          body: offer.sdp,
        });
        if (!r.ok) throw new Error(`WHEP ${r.status}`);
        await pc.setRemoteDescription({ type: 'answer', sdp: await r.text() });
      } catch {
        again();
      }
    };
    const again = () => {
      if (closed) return;
      setState('error');
      pc?.close();
      pc = null;
      window.clearTimeout(retry);
      retry = window.setTimeout(start, 3000);
    };
    start();
    return () => {
      closed = true;
      window.clearTimeout(retry);
      pc?.close();
    };
  }, [path]);

  return (
    <Box pos="relative" style={{ aspectRatio: String(ratio), background: '#000', borderRadius: 8, overflow: 'hidden' }}>
      <video ref={video} autoPlay muted playsInline style={{ width: '100%', height: '100%', objectFit: 'contain' }} />
      {state !== 'playing' && (
        <Center pos="absolute" inset={0} style={{ flexDirection: 'column', gap: 8 }}>
          {state === 'connecting' ? <Loader size="sm" color="gray" /> : <IconVideoOff size={28} color="gray" />}
          <Text size="xs" c="dimmed">
            {state === 'connecting' ? 'Connecting…' : 'Stream unavailable — retrying'}
          </Text>
        </Center>
      )}
    </Box>
  );
}
