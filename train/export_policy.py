"""Export a frozen actor and verify real native observations across Torch/ORT/C++.

Validation samples frozen-policy trajectories; no optimizer updates occur.
Use an isolated export environment with torch, onnx and onnxruntime installed.
"""
import argparse
import json
import subprocess
from pathlib import Path
import time

import numpy as np
import torch
import onnx
import onnxruntime as ort
import rts_native as R
from checkpointing import atomic_json, atomic_write, sha256
from ppo import Cfg, Policy, Observer, make_env
from checkpointing import load_weights


class Actor(torch.nn.Module):
    def __init__(self, policy):
        super().__init__()
        self.policy=policy

    def forward(self, cells, own, glob):
        return self.policy(cells.permute(0,3,1,2),own,glob)[0]


def export(args):
    torch.set_num_threads(1)
    cfg=Cfg(device='cpu',envs=1,threads=1,torch_threads=1,map_path=args.map_path,stats_path=args.stats_path)
    net=Policy(R.obs.K,R.obs.CHANNEL_COUNT,R.obs.SELF_COUNT,R.obs.GLOBAL_COUNT,R.obs.ACTION_COUNT)
    net.load_state_dict(load_weights(args.checkpoint))
    actor=Actor(net.eval()).eval()
    for parameter in actor.parameters(): parameter.requires_grad_(False)
    contract=json.loads(subprocess.check_output([args.probe,args.stats_path],text=True,encoding='utf-8'))
    assert contract['obs_version']==R.obs.VERSION
    assert int(contract['obs_fingerprint'])==R.obs.LAYOUT_FINGERPRINT
    types=args.unit_types.split(',')
    if not types or any(t not in R.obs.UNIT_TYPE_NAMES[5:] for t in types):
        raise ValueError('Declare the attacker types actually covered by training')
    if not 1 <= args.min_level <= args.max_level or not 4 <= args.ticks_per_step <= 8:
        raise ValueError('Invalid level/decision period contract')
    env=make_env(cfg,1,1.)
    obs=Observer(env)
    samples=[]
    with torch.inference_mode():
        for step in range(301):
            rows,c,s,g,m=obs.read()
            if len(rows)==0:break
            if step%50==0: samples.append((c.copy(),s.copy(),g.copy(),m.copy()))
            logits=actor(*(torch.from_numpy(x) for x in (c,s,g))).numpy()
            actions=np.zeros((1,obs.mu),np.uint8)
            actions.reshape(-1)[rows]=np.where(m,logits,-np.inf).argmax(-1)
            done=np.zeros(1,np.uint8)
            env.step(actions,done)
            if done[0]:break
    c,s,g,m=(np.concatenate([sample[i] for sample in samples]) for i in range(4))
    example=tuple(torch.from_numpy(x[:1]) for x in (c,s,g))
    output=Path(args.output)
    if output.exists():raise FileExistsError(f'Export is immutable: {output}')
    import io
    buffer=io.BytesIO()
    with torch.inference_mode():
        torch.onnx.export(actor,example,buffer,input_names=['cells','own','global'],output_names=['logits'],
            dynamic_axes={key:{0:'agents'} for key in ('cells','own','global','logits')},
            opset_version=17,dynamo=False,external_data=False)
    model=onnx.load_model_from_string(buffer.getvalue())
    metadata={'rts.format':'tactical-policy-1','rts.obs_version':str(R.obs.VERSION),
        'rts.obs_fingerprint':str(R.obs.LAYOUT_FINGERPRINT),'rts.stats_fingerprint':contract['stats_fingerprint'],
        'rts.cells_layout':'NHWC','rts.actions':str(R.obs.ACTION_COUNT),
        'rts.agent_semantics':'squad-leader-broadcast-v1','rts.unit_types':','.join(str(R.obs.UNIT_TYPE_NAMES.index(t)) for t in types),
        'rts.min_level':str(args.min_level),'rts.max_level':str(args.max_level),
        'rts.ticks_per_step':str(args.ticks_per_step),'rts.source_sha256':sha256(args.checkpoint),
        'rts.source_simulation_fingerprint':str(R.SIMULATION_FINGERPRINT)}
    onnx.helper.set_model_props(model,metadata)
    onnx.checker.check_model(model)
    options=ort.SessionOptions();options.intra_op_num_threads=1;options.inter_op_num_threads=1
    options.add_session_config_entry('session.intra_op.allow_spinning','0')
    runtime=ort.InferenceSession(model.SerializeToString(),options,providers=['CPUExecutionProvider'])
    checks=[]
    with torch.inference_mode():
        for count in (1,9,R.obs.MAX_UNITS_PER_ENV):
            indices=np.linspace(0,len(c)-1,count,dtype=int)
            arrays={name:np.ascontiguousarray(x[indices]) for name,x in zip(('cells','own','global'),(c,s,g))}
            expected=actor(*(torch.from_numpy(x) for x in arrays.values())).numpy()
            actual=runtime.run(['logits'],arrays)[0]
            np.testing.assert_allclose(actual,expected,rtol=1e-5,atol=1e-5)
            legal=m[indices]
            selected=np.where(legal,actual,-np.inf).argmax(-1)
            assert np.array_equal(selected,np.where(legal,expected,-np.inf).argmax(-1))
            checks.append(dict(agents=count,max_abs_error=float(np.max(np.abs(actual-expected))),actions_equal=True))
    atomic_write(output,lambda stream:stream.write(model.SerializeToString()))
    fixture=output.with_suffix('.input.json')
    atomic_json(fixture,{**{name:x.reshape(-1).tolist() for name,x in arrays.items()},
                         'masks':(legal.astype(np.uint16)*(1<<np.arange(R.obs.ACTION_COUNT))).sum(-1).tolist()})
    try:
        cpp=json.loads(subprocess.check_output([args.probe,args.stats_path,str(output),str(fixture)],text=True,encoding='utf-8'))
        np.testing.assert_allclose(np.array(cpp['logits']).reshape(expected.shape),expected,rtol=1e-5,atol=1e-5)
        assert cpp['actions']==selected.tolist()
        report={'model_sha256':sha256(output),'metadata':metadata,'checks':checks,
                'cpp_actions_equal':True,'cpp_mean_inference_ms':cpp['mean_inference_ms'],
                'cpp_model_identity':cpp['identity'],'torch':torch.__version__,'onnx':onnx.__version__,
                'onnxruntime':ort.__version__,'validation_map':args.map_path,
                'trajectory_observations':len(c),'trajectory_steps':step+1}
        atomic_json(output.with_suffix('.verification.json'),report)
    except BaseException:
        # A failed validation must never leave a deployable final model behind.
        output.unlink(missing_ok=True)
        raise
    print(json.dumps(report,ensure_ascii=False,indent=2))


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--checkpoint',required=True)
    parser.add_argument('--output',required=True)
    parser.add_argument('--probe',required=True,help='Compiled C++ policy_probe executable')
    parser.add_argument('--unit-types',required=True,help='Comma-separated trained attacker names')
    parser.add_argument('--min-level',type=int,required=True)
    parser.add_argument('--max-level',type=int,required=True)
    parser.add_argument('--ticks-per-step',type=int,default=6)
    parser.add_argument('--map-path',default=Cfg.map_path)
    parser.add_argument('--stats-path',default=Cfg.stats_path)
    export(parser.parse_args())


if __name__=='__main__':main()
