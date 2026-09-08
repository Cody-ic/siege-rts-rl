"""Full-campaign Python contract checks; run with the current native build on PYTHONPATH."""
from pathlib import Path
import unittest

import numpy as np
import rts_native as native

ROOT = Path(__file__).resolve().parents[1]


class CampaignBindingTests(unittest.TestCase):
    def setUp(self):
        self.campaign = native.TrainingCampaign(
            str(ROOT / "game/data/maps/pool/gen_01001000.json"),
            str(ROOT / "game/data/stats_placeholder.json"), 1)

    def test_observation_owns_memory_and_matches_registry(self):
        cells, global_values = self.campaign.defender_observation()
        self.assertEqual(cells.shape, (native.macro_obs.GRID, native.macro_obs.GRID,
                                      len(native.macro_obs.CELL_NAMES)))
        self.assertEqual(global_values.shape, (len(native.macro_obs.GLOBAL_NAMES),))
        self.assertEqual(cells.dtype, np.dtype("float32"))
        self.assertTrue(np.isfinite(cells).all())
        self.assertTrue(np.isfinite(global_values).all())
        snapshot = cells.copy()
        self.campaign.advance_scripted(20)
        np.testing.assert_array_equal(cells, snapshot)
        cells.fill(-999)
        self.assertTrue((self.campaign.defender_observation()[0] >= 0).all())

    def test_mask_is_read_only_and_summon_changes_phase(self):
        names = native.COMMAND_KIND_NAMES
        commands = [(names.index("None"), 65535, 0, 1),
                    (names.index("Summon"), 65535, 0, 1),
                    (names.index("Build"), 65535, 0, 1)]
        before = self.campaign.diagnostic_state_hash
        np.testing.assert_array_equal(self.campaign.command_mask(commands), [True, False, False])
        self.assertEqual(before, self.campaign.diagnostic_state_hash)
        self.campaign.advance(2, [])
        self.assertTrue(self.campaign.command_mask(commands)[1])
        branch = self.campaign.fork()
        branch.advance(1, [commands[1]])
        assault = native.macro_obs.GLOBAL_NAMES.index("assault")
        self.assertEqual(branch.defender_observation()[1][assault], 1)
        self.assertEqual(self.campaign.defender_observation()[1][assault], 0)
        self.assertFalse(branch.command_mask(commands)[1])
        before = branch.diagnostic_state_hash
        with self.assertRaises(Exception):
            branch.command_mask([(999, 0, 0, 1)])
        self.assertEqual(before, branch.diagnostic_state_hash)


if __name__ == "__main__":
    unittest.main()
