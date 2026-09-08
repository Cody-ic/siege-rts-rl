"""One immutable tactical opponent per training/evaluation invocation."""
import rts_native as native
from checkpointing import sha256


def load_opponent(path, stats):
    if not path:
        return None, dict(kind='script')
    digest=sha256(path)
    opponent=native.FrozenAttacker(str(path),str(stats))
    if sha256(path)!=digest:
        raise ValueError('Attacker model changed while loading')
    return opponent,dict(kind='frozen-tactical',sha256=digest,identity=opponent.identity)


def check_opponent(path, identity):
    if path and sha256(path)!=identity['sha256']:
        raise ValueError('Attacker model changed; committed checkpoints are preserved')
