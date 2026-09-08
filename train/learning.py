"""Training arithmetic independent of torch and the native module."""
from collections import deque
import math


def task_reward(economic: float, won: bool, mode: str, win_reward: float) -> float:
    """Called once per transition, before resetting completed environments.

    economic preserves the attrition objective; victory is an explicit alternative
    that excludes every repeatable tally reward. PBRS is added separately.
    """
    if mode not in ('economic', 'victory'):
        raise ValueError('unknown reward mode')
    if not math.isfinite(win_reward) or win_reward <= 0:
        raise ValueError('win_reward must be finite and positive')
    return (economic if mode == 'economic' else 0.0) + (win_reward if won else 0.0)


def potential_reward(before: float, after: float, done: bool, gamma: float) -> float:
    """Finite-episode PBRS: terminal states have zero potential.

    With the same gamma as GAE, the discounted sum is -Phi(initial),
    including deaths and timeout termination. Read `after` before reset.
    """
    return gamma * (0.0 if done else after) - before


class CompletionWindow:
    """Keep at least `minimum` episodes without slicing a completion batch.

    Simultaneous completions are valid samples. Cutting the last 100 rows of
    a 256-row batch biases the result towards high environment indices.
    This is a rolling training diagnostic, not a fixed-policy evaluation.
    """

    def __init__(self, minimum: int):
        if minimum < 1:
            raise ValueError('minimum must be positive')
        self.minimum = minimum
        self.clear()

    def clear(self):
        self.batches = deque()
        self.count = 0
        self.hits = 0

    def state_dict(self):
        return {'minimum': self.minimum, 'batches': list(self.batches)}

    def load_state_dict(self, state):
        if state['minimum'] != self.minimum:
            raise ValueError('completion window size differs')
        self.clear()
        for count, hits in state['batches']:
            if not 0 <= hits <= count or count < 1:
                raise ValueError('invalid completion batch')
            self.batches.append((count, hits))
            self.count += count
            self.hits += hits

    def add(self, damage):
        batch = tuple(damage)
        if not batch:
            return
        # np.bool_ sums produce np.int64, which isn't weights_only-safe or JSON
        # serializable. Checkpoint windows contain ordinary Python integers.
        entry = (len(batch), sum(int(x > 0) for x in batch))
        self.batches.append(entry)
        self.count += entry[0]
        self.hits += entry[1]
        while self.count - self.batches[0][0] >= self.minimum:
            count, hits = self.batches.popleft()
            self.count -= count
            self.hits -= hits

    @property
    def ready(self):
        return self.count >= self.minimum

    @property
    def rate(self):
        return self.hits / self.count if self.count else 0.0
