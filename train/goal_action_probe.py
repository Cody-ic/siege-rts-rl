"""Read-only action diagnostics on the existing frozen goal-coverage evaluator.

Accepts the same arguments as goal_coverage.py. Actions, RNG and rewards are
unchanged; compare resulting case records against the uninstrumented run.
"""
import argparse
import json
from pathlib import Path
import numpy as np
import rts_native as R
import goal_coverage as coverage
from checkpointing import atomic_json, sha256
from diagnostics import EpisodeDiagnostics


def main():
    parser=argparse.ArgumentParser(add_help=False)
    parser.add_argument('--output',required=True)
    args,_=parser.parse_known_args()
    original_env,original_forward=coverage.make_env,coverage.policy_forward
    holder={}

    def make_env(*pos,**kw):
        env=original_env(*pos,**kw)
        holder['env']=env
        holder['probes']=[EpisodeDiagnostics(p,25) for p in env.potentials]
        holder['entropy']=np.zeros(env.batch_size,np.float64)
        return env

    def forward(net,observation,device):
        result=original_forward(net,observation,device)
        dist,_=result
        if dist is not None:
            env=holder['env']
            selected=dist.logits.argmax(-1).cpu().numpy().astype(np.uint8)
            probabilities=dist.probs.cpu().numpy()
            entropy=dist.entropy().cpu().numpy()
            for i,probe in enumerate(holder['probes']):
                if env.episode_ends[i]!=R.EpisodeEnd.Running:continue
                where=observation[0]//R.obs.MAX_UNITS_PER_ENV==i
                probe.before_step(observation[1][where],observation[4][where],selected[where],probabilities[where])
                holder['entropy'][i]+=float(entropy[where].sum(dtype=np.float64))
        return result

    coverage.make_env,coverage.policy_forward=make_env,forward
    try:coverage.main()
    finally:coverage.make_env,coverage.policy_forward=original_env,original_forward
    report=json.loads(Path(args.output).read_text())
    for row,probe,entropy in zip(report['rows'],holder['probes'],holder['entropy']):
        data=probe.report()
        data['mean_entropy']=float(entropy/max(1,data['agent_decisions']))
        row['action_diagnostics']=data
    report['diagnostic_sources']={name:sha256(Path(__file__).parent/name) for name in ('goal_action_probe.py','diagnostics.py','goal_coverage.py')}
    atomic_json(args.output,report)


if __name__=='__main__':main()
