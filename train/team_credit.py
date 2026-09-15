"""Team credit with distinct world boundaries and actor liveness.

The critic sees only pooled policy observations, never hidden native state.
This first integration retains the existing finite single-wave task.
"""
import numpy as np
import torch
from torch import nn


def features(observer):
    live = observer.keys != 0
    count = live.sum(axis=1)
    # Mean/max of each local channel, then pool across live squads. This is a
    # deliberately coarse permutation-invariant baseline, not a full city state.
    local = np.concatenate((observer.cells.mean(axis=(2, 3)),
                            observer.cells.max(axis=(2, 3)), observer.own), axis=-1)
    pooled = (local * live[..., None]).sum(axis=1) / np.maximum(count[:, None], 1)
    return np.concatenate((pooled, observer.glob, count[:, None] / observer.mu), axis=1).astype(np.float32)


class TeamValue(nn.Module):
    def __init__(self, channels, own, glob):
        super().__init__()
        self.size = 2 * channels + own + glob + 1
        # Keep policy initialization/sampling RNG identical between paired arms.
        with torch.random.fork_rng(devices=[]):
            self.net = nn.Sequential(nn.Linear(self.size, 128), nn.Tanh(),
                                     nn.Linear(128, 128), nn.Tanh(), nn.Linear(128, 1))

    def forward(self, inputs):
        return self.net(inputs).squeeze(-1)


def advantages(values, rewards, next_values, terminated, truncated, gamma, lam):
    """All arrays (time, environments), final-state values supplied before reset.

    Truncation bootstraps but stops recursion into a reset world. End of a
    rollout bootstraps naturally. Squad death and wave changes are not inputs.
    """
    if any(x.shape != rewards.shape for x in (values, next_values, terminated, truncated)):
        raise ValueError('Team GAE shapes differ')
    result = torch.zeros_like(values)
    tail = torch.zeros_like(values[0])
    for t in reversed(range(len(values))):
        bootstrap = ~terminated[t].bool()
        continue_trace = bootstrap & ~truncated[t].bool()
        delta = rewards[t] + gamma * next_values[t] * bootstrap - values[t]
        tail = delta + gamma * lam * tail * continue_trace
        result[t] = tail
    return result


def update(value_net, optimizer, inputs, targets, epochs, coefficient, max_grad_norm):
    losses = []
    # Exactly one sample per environment step, including zero-actor tails.
    for _ in range(epochs):
        loss = .5 * (value_net(inputs) - targets).square().mean()
        if not torch.isfinite(loss):
            raise FloatingPointError('Nonfinite team value loss')
        optimizer.zero_grad(set_to_none=True)
        (coefficient * loss).backward()
        nn.utils.clip_grad_norm_(value_net.parameters(), max_grad_norm, error_if_nonfinite=True)
        optimizer.step()
        losses.append(float(loss.detach()))
    return float(np.mean(losses))
