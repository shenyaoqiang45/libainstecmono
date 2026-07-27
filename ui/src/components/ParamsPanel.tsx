import { useEffect, useState } from 'react';
import { Ros, Service, ServiceRequest } from 'roslib';

// Camera attributes live as dynamic ROS params named "attr.<Key>" on the camera
// node. We talk to the node's own rcl_interfaces services directly instead of
// rosapi: this rosapi build crashes on get_param_names (it enumerates every
// node and dies on nodes without parameter services).
const NODE = '/ainstec_camera';

// rcl_interfaces/msg/ParameterType
const TYPE_BOOL = 1;
const TYPE_INTEGER = 2;
const TYPE_DOUBLE = 3;

interface ParamEntry {
  name: string; // e.g. attr.Exposure
  pvType: number; // ParameterValue.type
  value: unknown;
}

export default function ParamsPanel({ ros }: { ros: Ros | null }) {
  const [params, setParams] = useState<ParamEntry[]>([]);
  const [status, setStatus] = useState<'idle' | 'loading' | 'error'>('idle');
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    if (!ros) {
      setParams([]);
      setStatus('idle');
      return;
    }
    let cancelled = false;
    setStatus('loading');
    setError(null);

    const listSrv = new Service({
      ros,
      name: `${NODE}/list_parameters`,
      serviceType: 'rcl_interfaces/srv/ListParameters',
    });
    listSrv.callService(
      new ServiceRequest({ prefixes: [], depth: 0 }),
      (listResp: any) => {
        if (cancelled) return;
        const names: string[] = (listResp?.result?.names ?? [])
          .filter((n: string) => n.startsWith('attr.'))
          .sort();
        if (names.length === 0) {
          setParams([]);
          setStatus('idle');
          return;
        }
        const getSrv = new Service({
          ros,
          name: `${NODE}/get_parameters`,
          serviceType: 'rcl_interfaces/srv/GetParameters',
        });
        getSrv.callService(
          new ServiceRequest({ names }),
          (getResp: any) => {
            if (cancelled) return;
            const values: any[] = getResp?.values ?? [];
            const entries: ParamEntry[] = names.map((name, i) => {
              const pv = values[i] ?? {};
              return { name, pvType: pv.type ?? 0, value: paramValueToJs(pv) };
            });
            setParams(entries);
            setStatus('idle');
          },
          (err: any) => {
            if (cancelled) return;
            setError(`get_parameters failed: ${stringifyErr(err)}`);
            setStatus('error');
          }
        );
      },
      (err: any) => {
        if (cancelled) return;
        setError(`list_parameters failed: ${stringifyErr(err)}`);
        setStatus('error');
      }
    );

    return () => {
      cancelled = true;
    };
  }, [ros]);

  if (!ros) return <p className="muted">Connect to rosbridge first.</p>;
  if (status === 'error') return <p className="error-text">{error}</p>;
  if (status === 'loading') return <p className="muted">Loading parameters…</p>;
  if (params.length === 0) return <p className="muted">No attr.* parameters found.</p>;

  return (
    <div className="panel-body params-list">
      {params.map((entry) => (
        <ParamRow key={`${entry.name}:${String(entry.value)}`} ros={ros} entry={entry} />
      ))}
    </div>
  );
}

function paramValueToJs(pv: any): unknown {
  switch (pv?.type) {
    case TYPE_BOOL:
      return !!pv.bool_value;
    case TYPE_INTEGER:
      return Number(pv.integer_value);
    case TYPE_DOUBLE:
      return Number(pv.double_value);
    case 4:
      return pv.string_value;
    default:
      return undefined;
  }
}

function stringifyErr(err: any): string {
  return typeof err === 'string' ? err : JSON.stringify(err);
}

function ParamRow({ ros, entry }: { ros: Ros; entry: ParamEntry }) {
  const [error, setError] = useState<string | null>(null);

  const commit = (value: number | boolean) => {
    let pv: Record<string, unknown>;
    if (entry.pvType === TYPE_BOOL) {
      pv = { type: TYPE_BOOL, bool_value: !!value };
    } else if (entry.pvType === TYPE_INTEGER) {
      pv = { type: TYPE_INTEGER, integer_value: Math.round(Number(value)) };
    } else {
      pv = { type: TYPE_DOUBLE, double_value: Number(value) };
    }
    const setSrv = new Service({
      ros,
      name: `${NODE}/set_parameters`,
      serviceType: 'rcl_interfaces/srv/SetParameters',
    });
    setSrv.callService(
      new ServiceRequest({ parameters: [{ name: entry.name, value: pv }] }),
      (resp: any) => {
        const result = resp?.results?.[0];
        if (result && result.successful === false) {
          setError(result.reason || 'set rejected');
        } else {
          setError(null);
        }
      },
      (err: any) => setError(stringifyErr(err))
    );
  };

  let input;
  if (entry.pvType === TYPE_BOOL) {
    input = (
      <input
        type="checkbox"
        defaultChecked={!!entry.value}
        onChange={(e) => commit(e.target.checked)}
      />
    );
  } else if (entry.pvType === TYPE_INTEGER || entry.pvType === TYPE_DOUBLE) {
    input = (
      <input
        type="number"
        defaultValue={entry.value as number}
        step="any"
        onKeyDown={(e) => {
          if (e.key === 'Enter') {
            const v = parseFloat((e.target as HTMLInputElement).value);
            if (!Number.isNaN(v)) commit(v);
          }
        }}
        onBlur={(e) => {
          const v = parseFloat(e.target.value);
          if (!Number.isNaN(v) && v !== entry.value) commit(v);
        }}
      />
    );
  } else {
    input = <span className="muted">{String(entry.value)}</span>;
  }

  return (
    <div className="param-row">
      <label title={entry.name}>{entry.name}</label>
      {input}
      {error && <span className="error-text">{error}</span>}
    </div>
  );
}
