import { IDEAL, type ChallengeState } from './core/challenge';

export interface ChallengePanelProps {
  challenge: ChallengeState;
  onChange: (next: ChallengeState) => void;
}

export default function ChallengePanel({ challenge, onChange }: ChallengePanelProps) {
  const set = (patch: Partial<ChallengeState>) => onChange({ ...challenge, ...patch });
  return (
    <div className="challenge-panel">
      <label className="sim-check">
        <input
          type="checkbox"
          checked={challenge.materialEffects}
          onChange={(e) => set({ materialEffects: e.target.checked })}
        />
        材质效果（暗面/金属丢失、玻璃无深度）
      </label>
      <label className="sim-check">
        <input
          type="checkbox"
          checked={challenge.interference}
          onChange={(e) => set({ interference: e.target.checked })}
        />
        双机主动干扰
      </label>
      <label className="sim-check">
        环境光噪声
        <input
          type="range"
          min={0}
          max={1}
          step={0.05}
          value={challenge.ambientLight}
          onChange={(e) => set({ ambientLight: parseFloat(e.target.value) })}
        />
        <span>{(challenge.ambientLight * 100).toFixed(0)}%</span>
      </label>
      <button type="button" className="secondary" onClick={() => onChange(IDEAL)}>
        重置理想条件
      </button>
    </div>
  );
}
