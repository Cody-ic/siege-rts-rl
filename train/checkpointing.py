"""Crash-safe training checkpoints. Only tensors and weights_only-safe primitives.

The learner resumes at a completed PPO update. Active native episodes are restarted
with fresh episode seeds, explicitly counted; this is not a bit-exact world snapshot.
"""
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import random
import tempfile
import warnings

import numpy as np
import torch

FORMAT = 1
ROOT = Path(__file__).resolve().parents[1]


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def contract(native, cfg):
    # Compiled-in source identity catches stale bindings and allows equivalent rebuilds.
    return {
        'obs_version': native.obs.VERSION,
        'obs_fingerprint': native.obs.LAYOUT_FINGERPRINT,
        'actions': native.obs.ACTION_COUNT,
        'max_agents': native.obs.MAX_UNITS_PER_ENV,
        'map_sha256': sha256(cfg.map_path), 'stats_sha256': sha256(cfg.stats_path),
        'map_pool_sha256': [sha256(path) for path in cfg.map_pool],
        'simulation_fingerprint': native.SIMULATION_FINGERPRINT,
        'native_build_mode': native.BUILD_MODE,
        'tally_names': list(native.obs.TALLY_NAMES),
        'learner_version': 7,  # expanded audit tally layout; old runs require their frozen binding
        'learner_sha256': {name: sha256(ROOT/'train'/name) for name in
                           ('ppo.py', 'learning.py', 'rollout.py', 'checkpointing.py', 'stable_kl.py')},
    }


def rng_state():
    ns = np.random.get_state()
    return {'python': random.getstate(), 'numpy': [ns[0], ns[1].tolist(), *ns[2:]],
            'torch': torch.get_rng_state(),
            'cuda': torch.cuda.get_rng_state_all() if torch.cuda.is_available() else []}


def restore_rng(state):
    random.setstate(state['python'])
    ns = state['numpy']
    np.random.set_state((ns[0], np.asarray(ns[1], dtype=np.uint32), *ns[2:]))
    torch.set_rng_state(state['torch'].cpu())
    if torch.cuda.is_available() and state['cuda']:
        if len(state['cuda']) != torch.cuda.device_count():
            raise ValueError('CUDA device count differs from checkpoint; use --init-weights for a new experiment')
        torch.cuda.set_rng_state_all([s.cpu() for s in state['cuda']])


def _sync_dir(path):
    if os.name != 'nt':
        fd = os.open(path, os.O_RDONLY)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)


def atomic_write(path, writer):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=path.name + '.', suffix='.tmp', dir=path.parent)
    try:
        with os.fdopen(fd, 'wb') as stream:
            writer(stream)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        _sync_dir(path.parent)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def atomic_json(path, value):
    payload = json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False).encode('utf-8')
    atomic_write(path, lambda stream: stream.write(payload))


def save_run(folder, payload):
    folder = Path(folder)
    metadata = {k:payload[k] for k in
                ('format','config','contract','progress','status','initialization')}
    json.dumps(metadata,allow_nan=False)  # fail before touching a good generation
    latest, previous = folder / 'latest.pt', folder / 'previous.pt'
    # Complete and fsync a new generation before touching either good checkpoint.
    staged = folder / 'pending.pt'
    atomic_write(staged, lambda stream: torch.save(payload, stream))
    load_training(staged)  # verify weights_only readability before publishing
    if latest.exists():
        # Do not replace a valid fallback with a corrupt latest after recovery.
        try:
            load_training(latest)
        except Exception as exc:
            warnings.warn(f'Keeping previous checkpoint; invalid latest: {exc}')
        else:
            os.replace(latest, previous)
    os.replace(staged, latest)
    _sync_dir(folder)
    # These are convenience exports. latest.pt is the single authoritative state.
    atomic_write(folder / 'policy.pt', lambda stream: torch.save(payload['model'], stream))
    atomic_json(folder / 'state.json', metadata)


def load_training(path):
    data = torch.load(path, map_location='cpu', weights_only=True)
    required = {'format', 'model', 'optimizer', 'config', 'contract', 'progress', 'rng', 'status', 'initialization'}
    if not isinstance(data, dict) or not required.issubset(data) or data['format'] != FORMAT:
        raise ValueError('Not a resumable checkpoint. For old policy-only files use --init-weights PATH.')
    if data['config'].get('value_features')=='independent':
        if not isinstance(data.get('value_model'),dict) or not isinstance(data.get('value_optimizer'),dict):
            raise ValueError('Missing independent value checkpoint state')
        if not data['value_model'] or any(not isinstance(v,torch.Tensor) or not torch.isfinite(v).all()
                                          for v in data['value_model'].values()):
            raise ValueError('Invalid independent value weights')
    return data


def load_auto(folder):
    errors = []
    for name in ('latest.pt', 'previous.pt'):
        path = Path(folder) / name
        if not path.exists():
            continue
        try:
            data = load_training(path)
        except Exception as exc:
            errors.append(f'{name}: {exc}')
            continue
        if errors:
            warnings.warn('Falling back to previous.pt: ' + '; '.join(errors))
        return data, path
    if errors:
        raise ValueError('No readable checkpoint; refusing to restart silently: ' + '; '.join(errors))
    if any((Path(folder)/name).exists() for name in ('state.json', 'policy.pt', 'metrics.jsonl', 'config.json')):
        raise ValueError('Existing run has no checkpoint; refusing to restart silently')
    return None, None


def load_weights(path):
    data = torch.load(path, map_location='cpu', weights_only=True)
    return data['model'] if isinstance(data, dict) and 'format' in data else data


def repair_metrics(folder, checkpoint_step):
    """A killed process may leave a torn final line or metrics newer than latest.pt.
    Keep only committed updates, so resumed learning curves never double-count.
    """
    path = Path(folder)/'metrics.jsonl'
    if not path.exists():
        return
    raw = path.read_bytes()
    lines = raw.splitlines()
    kept = []
    for i, line in enumerate(lines):
        try:
            row = json.loads(line)
        except (ValueError, UnicodeDecodeError):
            if i != len(lines)-1:
                raise ValueError('Corrupt metrics before final line; inspect the run directory')
            break
        if row['env_steps'] <= checkpoint_step:
            kept.append(line)
    if kept != lines or (kept and not raw.endswith(b'\n')):
        atomic_write(path,lambda stream:stream.write(b'\n'.join(kept)+b'\n' if kept else b''))


@contextmanager
def run_lock(folder):
    """OS lock, released on exit/crash/reboot; no stale PID lock to delete."""
    folder = Path(folder)
    folder.mkdir(parents=True, exist_ok=True)
    with (folder / '.lock').open('a+b') as stream:
        if stream.tell() == 0:
            stream.write(b'0')
            stream.flush()
        stream.seek(0)
        try:
            if os.name == 'nt':
                import msvcrt
                msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as exc:
            raise RuntimeError(f'Run directory is already in use: {folder}') from exc
        try:
            yield
        finally:
            stream.seek(0)
            if os.name == 'nt':
                msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(stream, fcntl.LOCK_UN)
