"""Synthetic flow-following fixtures; no learned policy or performance claim."""
from pathlib import Path
import numpy as np
import onnx
from onnx import helper as h,numpy_helper as nh,TensorProto as T

root=Path(__file__).resolve().parents[1]
old=onnx.load(root/'game/testdata/rl_constant.onnx')
props={p.key:p.value for p in old.metadata_props}
inputs=list(old.graph.input)
channels=inputs[0].type.tensor_type.shape.dim[3].dim_value
weights=np.zeros((channels,13),np.float32);bias=np.full(13,-99,np.float32);bias[0]=0
directions=[(-1,-1),(0,-1),(1,-1),(1,0),(1,1),(0,1),(-1,1),(-1,0)]
for action,(di,dj) in enumerate(directions,1):
    weights[-2,action]=2*di;weights[-1,action]=2*dj;bias[action]=-(di*di+dj*dj)
middle=inputs[0].type.tensor_type.shape.dim[1].dim_value//2
nodes=[h.make_node('Gather',['cells','middle'],['row'],axis=1),
       h.make_node('Gather',['row','middle'],['cell'],axis=1),
       h.make_node('MatMul',['cell','weights'],['scores']),h.make_node('Add',['scores','bias'],['logits'])]
graph=h.make_graph(nodes,'synthetic-flow-goal',inputs,[h.make_tensor_value_info('logits',T.FLOAT,['agents',13])],
    initializer=[nh.from_array(np.array(middle,np.int64),'middle'),nh.from_array(weights,'weights'),nh.from_array(bias,'bias')])
for suffix,format in [('legacy','tactical-policy-1'),('macro','tactical-policy-2')]:
    model=h.make_model(graph,opset_imports=[h.make_opsetid('',17)],ir_version=9)
    metadata=dict(props);metadata['rts.format']=format
    if suffix=='macro':metadata['rts.goal_semantics']='macro-flow-v1'
    h.set_model_props(model,metadata);onnx.checker.check_model(model)
    onnx.save(model,root/f'game/testdata/rl_flow_{suffix}.onnx')
