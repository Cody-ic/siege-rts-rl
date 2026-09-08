"""Autoregressive defender commands with exact legality and replayable log probabilities.

Commands retain the native (kind, slot, what, level) encoding. Selection order is
kind -> what -> level -> slot. No candidate is ranked or discarded by a script.
This module does not define rewards, campaign resets or optimizer checkpoints.
"""
from dataclasses import dataclass

import numpy as np
import torch
from torch import nn
from torch.distributions import Categorical


@dataclass
class Decision:
    command: tuple
    log_prob: torch.Tensor
    value: torch.Tensor
    # Conditional entropies along the selected path, not exact joint entropy.
    path_entropy: torch.Tensor
    domains: tuple  # Four legal value arrays along this state's selected path.


class MacroPolicy(nn.Module):
    VERSION = 1
    ORDER = (0, 2, 3, 1)

    def __init__(self, cell_channels, global_count, command_count, hidden=64):
        super().__init__()
        self.contract = dict(version=self.VERSION, cell_channels=cell_channels,
                             global_count=global_count, command_count=command_count, hidden=hidden)
        self.encoder = nn.Sequential(nn.Conv2d(cell_channels, hidden, 3, padding=1), nn.SiLU(),
                                     nn.Conv2d(hidden, hidden, 3, padding=1), nn.SiLU())
        self.context = nn.Sequential(nn.Linear(hidden + global_count, hidden), nn.Tanh())
        self.kind_embedding = nn.Embedding(command_count, hidden)
        self.what_embedding = nn.Embedding(256, hidden)
        self.level_embedding = nn.Embedding(256, hidden)
        self.heads = nn.ModuleList(nn.Linear(hidden, n) for n in (command_count, 256, 256))
        self.position_query = nn.Linear(hidden, hidden)
        self.position_coords = nn.Linear(3, hidden, bias=False)
        self.critic = nn.Linear(hidden, 1)

    def encode(self, cells, global_values):
        device = next(self.parameters()).device
        cells = torch.as_tensor(cells, dtype=torch.float32, device=device)
        global_values = torch.as_tensor(global_values, dtype=torch.float32, device=device)
        if cells.ndim != 3 or cells.shape[-1] != self.contract['cell_channels']:
            raise ValueError('macro cells must have HWC shape matching the contract')
        if global_values.shape != (self.contract['global_count'],):
            raise ValueError('macro globals do not match the contract')
        spatial = self.encoder(cells.permute(2, 0, 1).unsqueeze(0))[0]
        context = self.context(torch.cat((spatial.mean(dim=(1, 2)), global_values)))
        return spatial, context

    def _position_logits(self, spatial, context, values, map_shape):
        height, width = map_shape
        if height < 1 or width < 1 or height * width > 65535:
            raise ValueError('invalid command map dimensions')
        slots = torch.as_tensor(values, dtype=torch.long, device=context.device)
        no_slot = slots == 65535
        if bool(((slots >= height * width) & ~no_slot).any()):
            raise ValueError('candidate slot is outside map')
        safe = slots.masked_fill(no_slot, 0)
        x, y = safe % width, safe // width
        gx = x * spatial.shape[2] // width
        gy = y * spatial.shape[1] // height
        local = spatial[:, gy, gx].T * (~no_slot).unsqueeze(1)
        coords = torch.stack(((x.float() + .5) / width,
                              (y.float() + .5) / height, no_slot.float()), dim=1)
        coords[:, :2] *= (~no_slot).unsqueeze(1)
        keys = local + self.position_coords(coords)
        return keys @ self.position_query(context) / context.numel() ** .5

    def decide(self, cells, global_values, candidates, map_shape, *, command=None, greedy=False):
        """Sample or score one command. Score with the ORIGINAL state's candidates.

        Recomputing candidates after advancing the world changes the distribution
        and invalidates PPO ratios. Greedy is conditional argmax, not joint MAP.
        """
        remaining = np.asarray(candidates)
        if remaining.ndim != 2 or remaining.shape[1] != 4 or len(remaining) == 0:
            raise ValueError('nonempty native candidate matrix required')
        if not np.issubdtype(remaining.dtype, np.integer):
            raise ValueError('candidate encoding must use integers')
        if (remaining < 0).any() or (remaining[:, 0] >= self.contract['command_count']).any():
            raise ValueError('invalid command kind')
        if (remaining[:, 2:] > 255).any() or (remaining[:, 3] < 1).any():
            raise ValueError('invalid command type or level')
        if command is not None:
            command = tuple(command)
            if len(command) != 4 or not np.any(np.all(remaining == command, axis=1)):
                raise ValueError('requested command is not legal in this state')
        def legal_values(stage, selected):
            subset = remaining
            for previous in self.ORDER[:stage]:
                subset = subset[subset[:, previous] == selected[previous]]
            return np.unique(subset[:, self.ORDER[stage]])
        return self._walk(cells, global_values, map_shape, legal_values, command, greedy)

    def rescore(self, cells, global_values, command, domains, map_shape):
        """PPO evaluation from the four saved original conditional masks.

        Store these arrays, not the complete (potentially 170k-row) command set.
        They are rollout data and must not be regenerated from a later world.
        """
        if len(domains) != 4 or len(command) != 4:
            raise ValueError('four conditional domains and command fields required')
        def legal_values(stage, selected):
            values = np.asarray(domains[stage])
            if values.ndim != 1 or len(values) == 0 or not np.issubdtype(values.dtype, np.integer):
                raise ValueError('invalid saved conditional domain')
            if not np.array_equal(values, np.unique(values)) or command[self.ORDER[stage]] not in values:
                raise ValueError('saved domain must be sorted, unique and include the chosen value')
            limit = self.contract['command_count'] if stage == 0 else 256 if stage < 3 else 65536
            if (values < (1 if stage == 2 else 0)).any() or (values >= limit).any():
                raise ValueError('saved conditional domain is out of range')
            return values
        return self._walk(cells, global_values, map_shape, legal_values, command, False)

    def _walk(self, cells, global_values, map_shape, legal_values, command, greedy):
        spatial, context = self.encode(cells, global_values)
        value = self.critic(context).squeeze(-1)
        log_prob = context.new_zeros(())
        entropy = context.new_zeros(())
        selected = [0] * 4
        domains = []
        embeddings = (self.kind_embedding, self.what_embedding, self.level_embedding)
        for stage, column in enumerate(self.ORDER):
            values = legal_values(stage, selected)
            domains.append(values.copy())
            indices = torch.as_tensor(values, dtype=torch.long, device=context.device)
            logits = (self.heads[stage](context)[indices] if stage < 3 else
                      self._position_logits(spatial, context, values, map_shape))
            distribution = Categorical(logits=logits)
            if command is not None:
                choice = torch.tensor(int(np.searchsorted(values, command[column])), device=context.device)
            else:
                choice = logits.argmax() if greedy else distribution.sample()
            selected[column] = int(values[int(choice)])
            log_prob = log_prob + distribution.log_prob(choice)
            entropy = entropy + distribution.entropy()
            if stage < 3:
                context = torch.tanh(context + embeddings[stage](indices[choice]))
        return Decision(tuple(selected), log_prob, value, entropy, tuple(domains))
