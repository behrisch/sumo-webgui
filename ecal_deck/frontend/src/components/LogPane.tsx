import { useEffect, useRef } from 'react';
import type { LogMessage } from '../generated/sumo';

const MAX_LINES = 200;

const LEVEL_COLOR: Record<string, string> = {
  ERROR:   '#f88',
  WARNING: '#fd8',
  INFO:    '#aaa',
};

interface Props {
  messages: LogMessage[];
}

export function LogPane({ messages }: Props) {
  const bottomRef = useRef<HTMLDivElement>(null);

  useEffect(() => {
    bottomRef.current?.scrollIntoView({ behavior: 'smooth' });
  }, [messages]);

  return (
    <div style={{
      position: 'absolute', bottom: 8, left: 8,
      width: 480, maxHeight: 160,
      background: 'rgba(0,0,0,0.7)', color: '#ccc',
      fontFamily: 'monospace', fontSize: 11,
      borderRadius: 6, overflow: 'hidden',
      display: 'flex', flexDirection: 'column',
    }}>
      <div style={{ overflowY: 'auto', flex: 1, padding: '4px 8px' }}>
        {messages.slice(-MAX_LINES).map((m, i) => (
          <div key={i} style={{ color: LEVEL_COLOR[m.level] ?? '#aaa', lineHeight: 1.5 }}>
            <span style={{ opacity: 0.5, marginRight: 6 }}>
              {new Date(Number(m.time_ms)).toISOString().slice(11, 19)}
            </span>
            {m.text}
          </div>
        ))}
        <div ref={bottomRef} />
      </div>
    </div>
  );
}
