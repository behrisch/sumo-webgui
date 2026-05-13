import { useEffect, useState } from 'react';
import toast from 'react-hot-toast';
import type { ListDirResponse } from '../generated/sumo';
import type { CommandResponse } from '../hooks/useSimSocket';

interface Props {
  sendCommand: (service: string, request?: Record<string, unknown>, onResponse?: (r: CommandResponse) => void) => void;
  onSelect: (path: string) => void;
  onCancel: () => void;
}

const style = {
  overlay: {
    position: 'fixed' as const, inset: 0,
    background: 'rgba(0,0,0,0.6)',
    display: 'flex', alignItems: 'center', justifyContent: 'center',
    zIndex: 1000,
  },
  box: {
    background: '#1e1e1e', color: '#ddd', fontFamily: 'monospace', fontSize: 12,
    borderRadius: 6, padding: 16, width: 520, maxHeight: '70vh',
    display: 'flex', flexDirection: 'column' as const, gap: 8,
    boxShadow: '0 4px 24px rgba(0,0,0,0.6)',
  },
  path: { color: '#aaa', wordBreak: 'break-all' as const },
  list: {
    overflowY: 'auto' as const, flex: 1,
    border: '1px solid #333', borderRadius: 4, padding: '4px 0',
  },
  item: (highlight: boolean): React.CSSProperties => ({
    padding: '3px 10px', cursor: 'pointer',
    background: highlight ? '#2a5f2a' : 'transparent',
    whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis',
  }),
  row: { display: 'flex', gap: 8 },
  btn: {
    background: '#333', color: '#fff', border: '1px solid #555',
    borderRadius: 3, cursor: 'pointer', padding: '3px 10px',
  },
};

export function FileBrowser({ sendCommand, onSelect, onCancel }: Props) {
  const [listing, setListing] = useState<ListDirResponse | null>(null);
  const [selected, setSelected] = useState<string | null>(null);

  const navigate = (path: string) => {
    setSelected(null);
    sendCommand('list_dir', { path }, (resp) => {
      const r = resp as unknown as ListDirResponse & { error?: string };
      if (r.error) { toast.error(r.error); return; }
      setListing(r);
    });
  };

  // eslint-disable-next-line react-hooks/exhaustive-deps -- navigate is stable (react-router guarantee)
  useEffect(() => { navigate(''); }, []);

  if (!listing) return null;

  const parent = listing.path.includes('/')
    ? listing.path.replace(/\/[^/]+$/, '') || '/'
    : null;

  const handleConfirm = () => {
    if (selected) onSelect(selected);
  };

  return (
    <div style={style.overlay} onClick={onCancel}>
      <div style={style.box} onClick={(e) => e.stopPropagation()}>
        <div style={style.path}>{listing.path}</div>

        <div style={style.list}>
          {parent !== null && (
            <div style={style.item(false)} onClick={() => navigate(parent)}>
              📁 ..
            </div>
          )}
          {(listing.dirs ?? []).map((d) => (
            <div key={d} style={style.item(false)}
              onClick={() => navigate(listing.path.replace(/\/$/, '') + '/' + d)}>
              📁 {d}
            </div>
          ))}
          {(listing.files ?? []).map((f) => {
            const full = listing.path.replace(/\/$/, '') + '/' + f;
            return (
              <div key={f} style={style.item(selected === full)}
                onClick={() => setSelected(full)}
                onDoubleClick={() => onSelect(full)}>
                📄 {f}
              </div>
            );
          })}
          {!(listing.dirs ?? []).length && !(listing.files ?? []).length && (
            <div style={{ padding: '4px 10px', color: '#666' }}>No .sumocfg files here</div>
          )}
        </div>

        <div style={style.row}>
          <button style={style.btn} onClick={handleConfirm} disabled={!selected}>Select</button>
          <button style={style.btn} onClick={onCancel}>Cancel</button>
          {selected && <span style={{ color: '#aaa', alignSelf: 'center', overflow: 'hidden', textOverflow: 'ellipsis' }}>{selected}</span>}
        </div>
      </div>
    </div>
  );
}
