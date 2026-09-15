"""Immutable opponents, rotating only between complete defender campaigns."""
from macro_opponent import load_opponent, check_opponent


class OpponentPool:
    def __init__(self, paths, stats):
        self.paths = tuple(paths)
        if not self.paths or len(set(self.paths)) != len(self.paths):
            raise ValueError('Opponent pool must be nonempty with unique paths')
        loaded = [load_opponent(path, stats) for path in self.paths]
        self.models = tuple(model for model, _ in loaded)
        self.identity = dict(kind='frozen-tactical-pool-v1',
                             schedule='complete-map-cycle-then-opponent-v1',
                             members=[identity for _, identity in loaded])

    def index(self, episode, map_count):
        if episode < 0 or map_count < 1:
            raise ValueError('Invalid campaign schedule')
        return (episode // map_count) % len(self.models)

    def select(self, episode, map_count):
        return self.models[self.index(episode, map_count)]

    def check(self):
        for path, identity in zip(self.paths, self.identity['members']):
            check_opponent(path, identity)
