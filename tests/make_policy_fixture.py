"""Regenerate the tiny synthetic inference fixture; it contains no trained weights."""
from pathlib import Path
import json
import subprocess
import sys
import onnx
from onnx import TensorProto,helper

root=Path(__file__).resolve().parents[1]
contract=json.loads(subprocess.check_output([sys.argv[1],str(root/'game/data/stats_placeholder.json')],text=True))
graph=helper.make_graph([
    helper.make_node('MatMul',['global','weights'],['base']),
    helper.make_node('Add',['base','bias'],['logits'])], 'synthetic-test-policy',
    [helper.make_tensor_value_info('cells',TensorProto.FLOAT,['agents',15,15,14]),
     helper.make_tensor_value_info('own',TensorProto.FLOAT,['agents',14]),
     helper.make_tensor_value_info('global',TensorProto.FLOAT,['agents',2])],
    [helper.make_tensor_value_info('logits',TensorProto.FLOAT,['agents',13])],
    [helper.make_tensor('weights',TensorProto.FLOAT,[2,13],[0.]*26),
     helper.make_tensor('bias',TensorProto.FLOAT,[13],list(range(13)))])
model=helper.make_model(graph,opset_imports=[helper.make_opsetid('',17)],ir_version=9)
helper.set_model_props(model,{'rts.format':'tactical-policy-1','rts.obs_version':str(contract['obs_version']),
    'rts.obs_fingerprint':contract['obs_fingerprint'],'rts.stats_fingerprint':contract['stats_fingerprint'],
    'rts.cells_layout':'NHWC','rts.actions':'13','rts.agent_semantics':'squad-leader-broadcast-v1',
    'rts.unit_types':'5,6,7,8,9,10','rts.min_level':'1','rts.max_level':'1000','rts.ticks_per_step':'6'})
onnx.checker.check_model(model)
onnx.save(model,root/'game/testdata/rl_constant.onnx')
