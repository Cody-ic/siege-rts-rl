"""Probability/gradient checks plus a real campaign action execution."""
from pathlib import Path
import sys
import unittest
import io

import numpy as np
import torch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'train'))
from macro_policy import MacroPolicy


class MacroPolicyTests(unittest.TestCase):
    def setUp(self):
        torch.set_num_threads(1)
        torch.manual_seed(17)
        self.policy = MacroPolicy(3, 2, 11, hidden=16)
        self.cells = np.zeros((4, 4, 3), np.float32)
        self.globals = np.zeros(2, np.float32)
        self.candidates = np.array([(0,65535,0,1), (1,0,1,1), (1,3,1,1),
                                    (1,2,2,1), (4,0,0,1), (4,0,0,2)])

    def test_joint_probability_is_normalized_and_order_independent(self):
        probabilities = []
        for command in self.candidates:
            a = self.policy.decide(self.cells, self.globals, self.candidates, (2,2), command=command)
            b = self.policy.decide(self.cells, self.globals, self.candidates[::-1].copy(), (2,2), command=command)
            torch.testing.assert_close(a.log_prob, b.log_prob)
            probabilities.append(a.log_prob.exp())
        self.assertAlmostEqual(float(torch.stack(probabilities).sum().detach()), 1, places=6)

    def test_sample_rescore_and_optimizer_update(self):
        decision = self.policy.decide(self.cells, self.globals, self.candidates, (2,2))
        rescored = self.policy.decide(self.cells, self.globals, self.candidates, (2,2), command=decision.command)
        torch.testing.assert_close(decision.log_prob, rescored.log_prob)
        compact = self.policy.rescore(self.cells, self.globals, decision.command, decision.domains, (2,2))
        torch.testing.assert_close(decision.log_prob, compact.log_prob)
        optimizer = torch.optim.Adam(self.policy.parameters(), lr=.01)
        before = float(rescored.log_prob.detach())
        (-rescored.log_prob + rescored.value.square()).backward()
        self.assertTrue(all(torch.isfinite(p.grad).all() for p in self.policy.parameters() if p.grad is not None))
        optimizer.step()
        after = self.policy.decide(self.cells, self.globals, self.candidates, (2,2), command=decision.command)
        self.assertGreater(float(after.log_prob.detach()), before)
        with self.assertRaises(ValueError):
            self.policy.decide(self.cells, self.globals, self.candidates, (2,2), command=(1,1,1,1))

    def test_model_optimizer_and_rng_roundtrip(self):
        optimizer = torch.optim.Adam(self.policy.parameters(), lr=.001)
        def update(policy, opt):
            d = policy.decide(self.cells, self.globals, self.candidates, (2,2))
            opt.zero_grad()
            (-d.log_prob + d.value.square()).backward()
            opt.step()
            return d.command
        update(self.policy, optimizer)
        stream = io.BytesIO()
        torch.save(dict(contract=self.policy.contract, model=self.policy.state_dict(),
                        optimizer=optimizer.state_dict(), rng=torch.get_rng_state()), stream)
        expected = update(self.policy, optimizer)
        stream.seek(0)
        saved = torch.load(stream, weights_only=True)
        restored = MacroPolicy(3, 2, 11, hidden=16)
        self.assertEqual(restored.contract, saved['contract'])
        restored.load_state_dict(saved['model'])
        restored_optimizer = torch.optim.Adam(restored.parameters(), lr=.001)
        restored_optimizer.load_state_dict(saved['optimizer'])
        torch.set_rng_state(saved['rng'])
        self.assertEqual(update(restored, restored_optimizer), expected)
        for key, value in self.policy.state_dict().items():
            torch.testing.assert_close(value, restored.state_dict()[key], rtol=0, atol=0)

    def test_actual_campaign_decisions_execute(self):
        import rts_native as native
        campaign = native.TrainingCampaign(str(ROOT/'game/data/maps/pool/gen_01001000.json'),
                                           str(ROOT/'game/data/stats_placeholder.json'), 1)
        policy = MacroPolicy(len(native.macro_obs.CELL_NAMES), len(native.macro_obs.GLOBAL_NAMES),
                             len(native.COMMAND_KIND_NAMES), hidden=16)
        for _ in range(6):
            with torch.no_grad():
                decision = policy.decide(*campaign.defender_observation(), campaign.candidates(), campaign.map_shape)
            self.assertTrue(campaign.command_mask([decision.command])[0])
            transition = campaign.advance(20, [decision.command])
            self.assertGreater(transition['ticks'], 0)
        self.assertEqual(campaign.tick, 120)


if __name__ == '__main__':
    unittest.main()
