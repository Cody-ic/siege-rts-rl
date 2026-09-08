"""Export cached defender encoder/decoder graphs and verify real native decisions.

This is an offline inference artifact, not a game runtime integration. Legality
and autoregressive selection remain outside the neural graphs, using native rules.
"""
import argparse
import io
from pathlib import Path

import numpy as np
import torch
import onnx
import onnxruntime as ort
import rts_native as native

from checkpointing import atomic_json,atomic_write,sha256
from macro_evaluate import load_policy


class Encoder(torch.nn.Module):
    def __init__(self,policy):
        super().__init__();self.policy=policy

    def forward(self,cells,glob,detail):
        p=self.policy
        spatial=p.encoder(cells.permute(2,0,1).unsqueeze(0))[0]
        context=p.context(torch.cat((spatial.mean((1,2)),glob)))
        fine=p.detail_encoder(detail.permute(2,0,1).unsqueeze(0))[0]
        context=torch.tanh(context+p.detail_context(torch.cat((fine.mean((1,2)),fine.amax((1,2))))))
        return spatial,context,fine,p.critic(context).squeeze(-1)


class Decoder(torch.nn.Module):
    def __init__(self,policy):
        super().__init__();self.policy=policy

    def forward(self,context,local,fine,coords,prefix):
        p=self.policy
        first=torch.tanh(context+p.kind_embedding(prefix[0]))
        second=torch.tanh(first+p.what_embedding(prefix[1]))
        third=torch.tanh(second+p.level_embedding(prefix[2]))
        keys=local+p.position_coords(coords)+p.detail_position(fine)
        logits=keys@p.position_query(third)/(p.contract['hidden']**.5)
        return p.heads[0](context),p.heads[1](first),p.heads[2](second),logits


def write_graph(module,inputs,path,input_names,output_names,dynamic_axes):
    buffer=io.BytesIO()
    torch.onnx.export(module,inputs,buffer,input_names=input_names,output_names=output_names,
                      dynamic_axes=dynamic_axes,opset_version=17,dynamo=False,external_data=False)
    graph=onnx.load_model_from_string(buffer.getvalue());onnx.checker.check_model(graph)
    atomic_write(path,lambda stream:stream.write(graph.SerializeToString()))


def position_inputs(spatial,fine,slots,shape):
    height,width=shape
    absent=slots==65535;safe=np.where(absent,0,slots)
    x=safe%width;y=safe//width
    local=spatial[:,y*spatial.shape[1]//height,x*spatial.shape[2]//width].T.copy()
    detail=fine[:,y,x].T.copy()
    coords=np.stack(((x+.5)/width,(y+.5)/height,absent),axis=1).astype(np.float32)
    local[absent]=0;detail[absent]=0;coords[absent,:2]=0
    return local,detail,coords


def export(checkpoint,maps,stats,output):
    if not maps:
        raise ValueError('At least one real validation map is required')
    checkpoint_sha256=sha256(checkpoint)
    output=Path(output)
    output.mkdir(parents=True,exist_ok=False)
    torch.set_num_threads(1)
    p,_,cfg=load_policy(checkpoint,stats)
    p.requires_grad_(False)
    encoder,decoder=Encoder(p).eval(),Decoder(p).eval()
    world=native.TrainingCampaign(str(maps[0]),str(stats),1)
    cells,glob=world.defender_observation();detail=world.defender_detail().astype(np.float16).astype(np.float32)
    hidden=p.contract['hidden']
    with torch.inference_mode():
        write_graph(encoder,tuple(torch.from_numpy(x) for x in (cells,glob,detail)),output/'encoder.onnx',
            ['cells','global','detail'],['spatial','context','fine','value'],
            {'detail':{0:'height',1:'width'},'fine':{1:'height',2:'width'}})
        write_graph(decoder,(torch.zeros(hidden),torch.zeros(1,hidden),torch.zeros(1,8),
                            torch.zeros(1,3),torch.tensor([0,0,1])),output/'decoder.onnx',
            ['context','local','fine','coords','prefix'],['kind','what','level','position'],
            {name:{0:'positions'} for name in ('local','fine','coords','position')})
    options=ort.SessionOptions();options.intra_op_num_threads=1;options.inter_op_num_threads=1
    enc=ort.InferenceSession(str(output/'encoder.onnx'),sess_options=options,providers=['CPUExecutionProvider'])
    dec=ort.InferenceSession(str(output/'decoder.onnx'),sess_options=options,providers=['CPUExecutionProvider'])
    checked=[]
    with torch.inference_mode():
        for path in maps:
            world=native.TrainingCampaign(str(path),str(stats),1)
            for step in range(4):
                cells,glob=world.defender_observation()
                detail=world.defender_detail().astype(np.float16).astype(np.float32)
                encoded=enc.run(None,{'cells':cells,'global':glob,'detail':detail})
                expected=encoder(*(torch.from_numpy(x) for x in (cells,glob,detail)))
                for actual,target in zip(encoded,expected):
                    np.testing.assert_allclose(actual,target.numpy(),rtol=2e-4,atol=2e-5)
                spatial,context,fine,value=encoded
                remaining=world.candidates();prefix=np.array([0,0,1],np.int64);command=[0]*4
                for stage,column in enumerate((0,2,3,1)):
                    values=np.unique(remaining[:,column])
                    slots=values if stage==3 else np.array([65535])
                    local,features,coords=position_inputs(spatial,fine,slots,world.map_shape)
                    inputs=dict(context=context,local=local,fine=features,coords=coords,prefix=prefix)
                    actual=dec.run(None,inputs)
                    expected=decoder(*(torch.from_numpy(inputs[name]) for name in ('context','local','fine','coords','prefix')))
                    for result,target in zip(actual,expected):
                        np.testing.assert_allclose(result,target.numpy(),rtol=2e-4,atol=2e-5)
                    logits=actual[stage] if stage==3 else actual[stage][values]
                    command[column]=int(values[logits.argmax()])
                    remaining=remaining[remaining[:,column]==command[column]]
                    if stage<3:prefix[stage]=command[column]
                expected=p.decide(cells,glob,world.candidates(),world.map_shape,detail=detail,greedy=True)
                if tuple(command)!=expected.command:
                    raise ValueError('Exported greedy command differs from PyTorch')
                checked.append(dict(map=str(path),tick=world.tick,command=command))
                world.advance(cfg['period'],[tuple(command)])
        # Exercise non-square dynamic resolutions independently of the map pool.
        alternate=np.zeros((41,53,len(native.macro_obs.DETAIL_NAMES)),np.float32)
        actual=enc.run(None,{'cells':cells,'global':glob,'detail':alternate})
        expected=encoder(torch.from_numpy(cells),torch.from_numpy(glob),torch.from_numpy(alternate))
        for result,target in zip(actual,expected):
            np.testing.assert_allclose(result,target.numpy(),rtol=2e-4,atol=2e-5)
        for prefix in ([4,3,8],[9,0,1],[0,255,255]):
            inputs=dict(context=context,local=np.ones((7,hidden),np.float32),
                fine=np.ones((7,8),np.float32),coords=np.zeros((7,3),np.float32),
                prefix=np.asarray(prefix,np.int64))
            actual=dec.run(None,inputs)
            expected=decoder(*(torch.from_numpy(inputs[name]) for name in ('context','local','fine','coords','prefix')))
            for result,target in zip(actual,expected):
                np.testing.assert_allclose(result,target.numpy(),rtol=2e-4,atol=2e-5)
    if sha256(checkpoint)!=checkpoint_sha256:
        raise ValueError('Checkpoint changed during export; refusing to publish a verified manifest')
    manifest=dict(format='defender-macro-onnx-v1',checkpoint_sha256=checkpoint_sha256,
        exporter_sha256=sha256(__file__),
        graphs={name:sha256(output/name) for name in ('encoder.onnx','decoder.onnx')},
        obs_version=native.macro_obs.VERSION,cells=list(native.macro_obs.CELL_NAMES),
        global_names=list(native.macro_obs.GLOBAL_NAMES),detail=list(native.macro_obs.DETAIL_NAMES),
        commands=list(native.COMMAND_KIND_NAMES),stats_sha256=sha256(stats),
        simulation=native.SIMULATION_FINGERPRINT,period=cfg['period'],hidden=hidden,
        detail_precision='float16-before-float32-inference',selection='conditional-greedy',
        no_slot=65535,checks=checked,dynamic_resolution_check=[41,53],
        decoder_prefix_checks=[[4,3,8],[9,0,1],[0,255,255]],game_runtime_integrated=False)
    atomic_json(output/'manifest.json',manifest)
    return manifest


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--checkpoint',required=True);parser.add_argument('--maps',nargs='+',required=True)
    parser.add_argument('--stats',default=str(Path(__file__).resolve().parents[1]/'game/data/stats_placeholder.json'))
    parser.add_argument('--output',required=True)
    args=parser.parse_args()
    export(args.checkpoint,args.maps,args.stats,args.output)
