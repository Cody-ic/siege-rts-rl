"""Live-row storage and identity-aware IPPO advantages."""
import numpy as np
import torch


class RolloutStorage:
    def __init__(self, steps, capacity, k, channels, own, glob, actions, pin=False):
        self.capacity = capacity
        self.c = torch.empty((steps, capacity, k, k, channels), pin_memory=pin)
        self.s = torch.empty((steps, capacity, own), pin_memory=pin)
        self.g = torch.empty((steps, capacity, glob), pin_memory=pin)
        self.m = torch.empty((steps, capacity, actions), dtype=torch.bool, pin_memory=pin)
        self.rows = np.zeros((steps, capacity), np.int64)
        self.counts = np.zeros(steps, np.int64)

    @property
    def bytes(self):
        return sum(t.numel() * t.element_size() for t in (self.c, self.s, self.g, self.m))

    def store(self, t, rows, c, s, g, m):
        count = len(rows)
        if count > self.capacity:
            raise ValueError('Roster grew beyond rollout capacity; increase the capacity before training')
        self.counts[t] = count
        self.rows[t, :count] = rows
        for dest, src in ((self.c, c), (self.s, s), (self.g, g), (self.m, m)):
            dest[t, :count].copy_(torch.from_numpy(src))

    def indices(self, steps, native_rows):
        packed = np.concatenate([t * self.capacity + np.arange(self.counts[t]) for t in range(steps)])
        native = np.concatenate([t * native_rows + self.rows[t, :self.counts[t]] for t in range(steps)])
        return packed, native


def advantages(values, rewards, dones, keys, next_value, gamma, gae_lambda):
    """Match surviving squads across compacted rows; dead agents don't bootstrap
    from a different squad. A leader replacement keeps the same squad identity.
    keys: (T+1, envs, max_agents), zero is padding, dones: (T, envs).
    """
    steps, envs, width = rewards.shape[0], keys.shape[1], keys.shape[2]
    matches = (keys[:-1, :, :, None] == keys[1:, :, None, :]) & (keys[:-1, :, :, None] != 0)
    alive = matches.any(-1) & ~dones.detach().cpu().numpy().astype(bool)[:, :, None]
    successor = matches.argmax(-1) + np.arange(envs)[None, :, None] * width
    successor = torch.as_tensor(successor.reshape(steps, -1), device=values.device)
    continuation = torch.as_tensor(alive.reshape(steps, -1), device=values.device)
    result = torch.zeros_like(values)
    last = torch.zeros_like(next_value)
    for t in reversed(range(steps)):
        nv = next_value if t == steps - 1 else values[t + 1]
        row = successor[t]
        delta = rewards[t].repeat_interleave(width) + gamma * nv[row] * continuation[t] - values[t]
        last = delta + gamma * gae_lambda * last[row] * continuation[t]
        result[t] = last
    return result
