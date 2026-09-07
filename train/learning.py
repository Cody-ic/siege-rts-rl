"""Training arithmetic independent of torch and the native module."""
from collections import deque


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

    def add(self, damage):
        batch = tuple(damage)
        if not batch:
            return
        entry = (len(batch), sum(x > 0 for x in batch))
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
