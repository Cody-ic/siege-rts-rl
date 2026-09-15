"""Read-only evaluation probes; never alter observations, actions, or rewards."""
import numpy as np

import rts_native as R

# Screen directions in the native isometric grid (rts/action.hpp).
MOVE_DELTAS = {'MoveN': (-1, -1), 'MoveNE': (0, -1), 'MoveE': (1, -1),
               'MoveSE': (1, 0), 'MoveS': (1, 1), 'MoveSW': (0, 1),
               'MoveW': (-1, 1), 'MoveNW': (-1, 0)}


def episode_rng(seed, episode):
    """A slot's replacement episode must not inherit another episode's RNG."""
    return np.random.default_rng(np.random.SeedSequence([seed, episode, 0x525453]))


def sample_actions(probabilities, rows, max_agents, generators):
    """Sample a frozen policy with independent random streams per episode."""
    selected = np.zeros(len(rows), np.uint8)
    for slot, generator in enumerate(generators):
        if generator is None:
            continue
        where = np.flatnonzero(rows // max_agents == slot)
        if not len(where):
            continue
        cdf = np.cumsum(probabilities[where], axis=1, dtype=np.float64)
        cdf /= cdf[:, -1:]
        selected[where] = (generator.random(len(where))[:, None] >= cdf).sum(axis=1)
    return selected


class EpisodeDiagnostics:
    def __init__(self, initial_potential, trace_every):
        self.counts = np.zeros(R.obs.ACTION_COUNT, np.int64)
        self.counters = dict(agent_decisions=0, no_legal_move=0, no_flow=0,
                             attack_available=0, attack_ignored=0,
                             moves_with_flow=0, moves_toward_flow=0,
                             moves_against_flow=0)
        self.first_contact = None
        self.initial_potential = float(initial_potential)
        self.trace_every = trace_every
        self.trace = []
        self.confidence_sum = 0.0
        self.probability_rows = 0

    def before_step(self, cells, legal, actions, probabilities=None):
        names = R.obs.ACTION_NAMES
        moves = [names.index(name) for name in MOVE_DELTAS]
        attacks = [i for i, name in enumerate(names) if name.startswith('Atk')]
        channels = [c[0] for c in R.obs.CHANNELS]
        vectors = cells[:, R.obs.K//2, R.obs.K//2][:,
                    [channels.index('flow_di'), channels.index('flow_dj')]]
        has_flow = np.any(vectors != 0, axis=1)
        available = legal[:, attacks].any(axis=1)
        delta = np.array([MOVE_DELTAS.get(names[int(a)], (0, 0)) for a in actions]).reshape(-1, 2)
        moving = np.isin(actions, moves) & has_flow
        dot = (delta * vectors).sum(axis=1)
        self.counts += np.bincount(actions, minlength=len(names))
        for name, values in (
            ('agent_decisions', np.ones(len(actions), bool)),
            ('no_legal_move', ~legal[:, moves].any(axis=1)), ('no_flow', ~has_flow),
            ('attack_available', available),
            ('attack_ignored', available & ~np.isin(actions, attacks)),
            ('moves_with_flow', moving), ('moves_toward_flow', moving & (dot > 0)),
            ('moves_against_flow', moving & (dot < 0))):
            self.counters[name] += int(values.sum())
        if probabilities is not None and len(actions):
            self.confidence_sum += float(probabilities.max(axis=1).sum(dtype=np.float64))
            self.probability_rows += len(actions)

    def after_step(self, step, tally, potential, live_squads, done):
        damage = float(tally[R.obs.TALLY_NAMES.index('dmg_to_blds')])
        if damage > 0 and self.first_contact is None:
            self.first_contact = step
        if step == 1 or step % self.trace_every == 0 or done:
            self.trace.append(dict(step=step, live_squads=int(live_squads),
                                   potential=float(potential), building_damage=damage,
                                   progress=float(tally[R.obs.TALLY_NAMES.index('progress')])))

    def report(self):
        return dict(action_counts=dict(zip(R.obs.ACTION_NAMES, self.counts.tolist())),
                    **self.counters, first_building_contact_step=self.first_contact,
                    initial_potential=self.initial_potential,
                    mean_top_probability=(self.confidence_sum/self.probability_rows
                                          if self.probability_rows else None),
                    trace=self.trace)


def summarize_spawns(rows):
    results = []
    for spawn in sorted({r['spawn_index'] for r in rows}):
        group = [r for r in rows if r['spawn_index'] == spawn]
        results.append(dict(spawn_index=spawn, episodes=len(group),
                            wins=sum(r['end'] == 'keep_destroyed' for r in group),
                            hit_buildings=sum(r['tally']['dmg_to_blds'] > 0 for r in group),
                            mean_building_damage=float(np.mean([r['tally']['dmg_to_blds'] for r in group])),
                            mean_progress=float(np.mean([r['tally']['progress'] for r in group]))))
    return results
