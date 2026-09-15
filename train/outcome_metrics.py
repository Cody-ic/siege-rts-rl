"""Raw outcome summaries, deliberately independent of reward weights."""
from reward_profiles import NEW_FIELDS


def summarize_outcomes(rows):
    if not rows:
        raise ValueError('Outcome summary requires completed episodes')
    names = ('blds_destroyed', 'units_killed', 'scout_units_killed', 'masons_killed',
             'phoenix_losses', 'enemy_unit_gold', 'opponent_repair_wood_spent', 'losses',
             'friendly_unit_damage', 'friendly_units_killed') + NEW_FIELDS
    # Missing diagnostics in historical reports are unknown, never zero.
    means = {name: (sum(row['tally'][name] for row in rows) / len(rows)
                   if all(name in row['tally'] for row in rows) else None)
             for name in names}
    return dict(mean=means, damage_without_destruction_episodes=sum(
        row['tally']['dmg_to_blds'] > 0 and row['tally']['blds_destroyed'] == 0
        for row in rows), semantics=dict(
            dmg_to_blds='Actual HP removed; not persistent economic loss',
            dmg_to_units='Enemy-only since learner version 9; earlier counters include friendly fire',
            units_killed='Enemy-only since learner version 9; earlier counters include friendly fire',
            friendly_unit_damage='Actual HP removed from own units; no positive reward',
            friendly_units_killed='Own units killed by own attacks; losses still records their full HP',
            enemy_unit_gold='Current replacement quote at killed defender type/level; not actual spending',
            opponent_repair_wood_spent='Actual prepaid repairs, including initial map damage; not attributed to attacker',
            losses='Own dead units maximum HP sum; not resource cost',
            phoenix_losses='Combat deaths only; retirement is not a death',
            scouts_killed='Legacy noncombat count includes masons; use scout_units_killed for scouts'))
